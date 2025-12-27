#define GL_SILENCE_DEPRECATION
#include <string>
#include <opencv2/opencv.hpp>
#include <ZXing/ReadBarcode.h>
#include <ZXing/Barcode.h>
#include <iostream>
#include <ZXing/ReaderOptions.h>
#include <unordered_set>
#include <chrono>
#include <ZXing/GTIN.h>
#include <algorithm>
#include <sstream>
#include "httplib.h"
#include "json.hpp"
#include "imgui.h"
#include <GLFW/glfw3.h>
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

enum class ScannerState {
    SCANNING,
    READY,
    SENDING,
    DONE,
    ERROR
};

const char* stateToStr(ScannerState s)
{
    switch (s) {
        case ScannerState::SCANNING: return "SCANNING";
        case ScannerState::READY:    return "READY";
        case ScannerState::SENDING:  return "SENDING";
        case ScannerState::DONE:     return "DONE";
        case ScannerState::ERROR:    return "ERROR";
        default: return "UNKNOWN";
    }
}

// Send aggregation request to FastAPI server
bool sendAggregation(const std::vector<std::string>& codes)
{
    using json = nlohmann::json;

    httplib::Client cli("http://127.0.0.1:8000");
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

    json payload;
    payload["device_id"] = "table_01";
    payload["codes"] = codes;

    auto res = cli.Post(
        "/api/v1/camera/scan/aggregation/test",
        payload.dump(),
        "application/json"
    );

    if (!res) {
        std::cerr << "❌ FastAPI not reachable\n";
        return false;
    }

    if (res->status != 200) {
        std::cerr << "❌ FastAPI error " << res->status << ":\n"
                  << res->body << std::endl;
        return false;
    }

    auto response = json::parse(res->body);

    std::cout << "✅ Aggregation started\n";
    std::cout << "Box ID: " << response.value("box_id", -1) << std::endl;
    std::cout << "SSCC: " << response.value("sscc_code", "") << std::endl;
    std::cout << "Order ID: " << response.value("order_id", -1) << std::endl;

    return true;
}

GLuint matToTexture(const cv::Mat& mat)
{
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GLenum format = mat.channels() == 3 ? GL_BGR : GL_LUMINANCE;

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGB,
        mat.cols,
        mat.rows,
        0,
        format,
        GL_UNSIGNED_BYTE,
        mat.data
    );

    return texture;
}

int main()
{
    // --- GLFW initialization ---
    if (!glfwInit()) {
        std::cerr << "❌ GLFW init failed\n";
        return 1;
    }

    GLFWwindow* window = glfwCreateWindow(1600, 900, "ZXing Scanner", nullptr, nullptr);
    if (!window) {
        std::cerr << "❌ GLFW window failed\n";
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // --- ImGui initialization ---
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    io.Fonts->AddFontFromFileTTF(
        "/System/Library/Fonts/SFNS.ttf",
        18.0f,
        nullptr,
        io.Fonts->GetGlyphRangesCyrillic()
    );

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 120");

    int cameraIndex = 0;

    cv::VideoCapture cap(cameraIndex, cv::CAP_AVFOUNDATION);
    if (!cap.isOpened()) {
        std::cerr << "❌ Cannot open camera\n";
        return 1;
    }

    std::cout << "▶ ZXing Scanner started (Q to quit)\n";

    ZXing::ReaderOptions hints;
    hints.setFormats(ZXing::BarcodeFormat::DataMatrix);
    hints.setTryHarder(true);

    std::unordered_set<std::string> seen;
    size_t scanIndex = 0;

    bool timingStarted = false;
    auto scanStartTime = std::chrono::steady_clock::time_point{};
    bool timingStopped = false;
    auto scanEndTime = std::chrono::steady_clock::time_point{};

    std::vector<std::string> scannedCodes;
    ScannerState state = ScannerState::SCANNING;
    bool justReset = false;

    double zoom = 1.0;          // 1.0 = no zoom
    const double zoomStep = 0.1;
    const double zoomMin = 1.0;
    const double zoomMax = 3.0;

    // --- fixed box & grid configuration ---
    const int GRID_ROWS = 2;
    const int GRID_COLS = 5;

    // Central box ROI as % of frame (tuned for fixed corner guides)
    const double BOX_W = 0.80;   // 80% width
    const double BOX_H = 0.70;   // 70% height

    const double CELL_ZOOM = 2.5; // aggressive local zoom per cell

    while (!glfwWindowShouldClose(window)) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty())
            break;

        // --- central BOX ROI ---
        int bw = static_cast<int>(frame.cols * BOX_W);
        int bh = static_cast<int>(frame.rows * BOX_H);
        int bx = (frame.cols - bw) / 2;
        int by = (frame.rows - bh) / 2;
        cv::Rect boxROI(bx, by, bw, bh);

        // draw box ROI
        cv::rectangle(frame, boxROI, cv::Scalar(255, 255, 0), 2);

        int cellW = bw / GRID_COLS;
        int cellH = bh / GRID_ROWS;

        for (int r = 0; r < GRID_ROWS; ++r) {
            for (int c = 0; c < GRID_COLS; ++c) {
                if (state != ScannerState::SCANNING || justReset)
                    continue;

                int cx = bx + c * cellW;
                int cy = by + r * cellH;
                cv::Rect cellROI(cx, cy, cellW, cellH);

                // draw grid
                cv::rectangle(frame, cellROI, cv::Scalar(200, 200, 200), 1);

                // skip already scanned cells by content (global dedup still applies)
                cv::Mat cell = frame(cellROI).clone();

                // local zoom for this cell
                cv::Mat cellZoomed;
                cv::resize(cell, cellZoomed, cv::Size(), CELL_ZOOM, CELL_ZOOM, cv::INTER_LINEAR);

                // grayscale
                cv::Mat gray;
                cv::cvtColor(cellZoomed, gray, cv::COLOR_BGR2GRAY);

                ZXing::ImageView image(
                    gray.data,
                    gray.cols,
                    gray.rows,
                    ZXing::ImageFormat::Lum
                );

                auto barcodes = ZXing::ReadBarcodes(image, hints);

                for (const auto& barcode : barcodes) {
                    if (!barcode.isValid() || barcode.text().empty())
                        continue;

                    const std::string& text = barcode.text();

                    // Normalize GS1
                    std::string normalized;
                    normalized.reserve(text.size());
                    for (char ch : text) {
                        if (ch != '(' && ch != ')')
                            normalized.push_back(ch);
                    }

                    if (seen.find(normalized) != seen.end())
                        continue;

                    if (!timingStarted) {
                        timingStarted = true;
                        scanStartTime = std::chrono::steady_clock::now();
                    }

                    seen.insert(normalized);
                    scannedCodes.push_back(normalized);
                    ++scanIndex;
                    if (state == ScannerState::SCANNING && scanIndex == 10 && !timingStopped) {
                        timingStopped = true;
                        scanEndTime = std::chrono::steady_clock::now();

                        state = ScannerState::READY;
                        std::cout << "🟢 State → READY (10/10 scanned)\n";
                    }
                    std::cout << scanIndex << ". " << normalized << std::endl;
                    std::cout << "\a" << std::flush;

                    // mark cell as scanned (green)
                    cv::rectangle(frame, cellROI, cv::Scalar(0, 255, 0), 3);
                }
            }
        }

        // --- overlay: scanned count ---
        // removed OpenCV text overlays

        // --- overlay: state ---
        // removed OpenCV text overlays

        // --- overlay: time when 10 codes scanned ---
        // removed OpenCV text overlays

        // --- State machine: handle READY state ---
        if (state == ScannerState::READY) {
            state = ScannerState::SENDING;
            std::cout << "📡 Sending aggregation request...\n";

            bool ok = sendAggregation(scannedCodes);
            if (ok) {
                state = ScannerState::DONE;
                std::cout << "✅ State → DONE\n";
            } else {
                state = ScannerState::ERROR;
                std::cout << "❌ State → ERROR\n";
            }
        }

        // NOTE: do NOT clear 'seen' automatically; scanned codes must not be rescanned

        GLuint tex = matToTexture(frame);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Camera window
        ImGui::Begin("Камера");
        // Preserve aspect ratio
        float availWidth = ImGui::GetContentRegionAvail().x;
        float aspect = (float)frame.rows / (float)frame.cols;
        ImVec2 imageSize(availWidth, availWidth * aspect);
        ImGui::Image((void*)(intptr_t)tex, imageSize);
        ImGui::End();

        // Info panel
        ImGui::Begin("Панель сканирования");
        ImGui::Text("📦 Коробки: %d", state == ScannerState::DONE ? 1 : 0);
        ImGui::Separator();
        ImGui::Text("🔢 Коды: %zu / 10", scanIndex);
        ImGui::ProgressBar(scanIndex / 10.0f, ImVec2(-1, 18));

        // --- Scan time for 10 codes ---
        if (timingStarted) {
            auto endTime = timingStopped
                ? scanEndTime
                : std::chrono::steady_clock::now();

            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                endTime - scanStartTime
            ).count();

            float seconds = elapsedMs / 1000.0f;

            ImGui::Spacing();
            ImGui::Text("⏱ Время сканирования: %.2f сек", seconds);
        }

        ImGui::Spacing();
        ImGui::Text("📄 Отсканированные коды:");

        ImGui::BeginChild("codes", ImVec2(0, 300), true);
        for (const auto& code : scannedCodes)
            ImGui::TextUnformatted(code.c_str());
        ImGui::EndChild();

        if (ImGui::Button("🔄 Сбросить", ImVec2(-1, 40))) {
            seen.clear();
            scanIndex = 0;
            scannedCodes.clear();

            timingStarted = false;
            timingStopped = false;
            scanStartTime = std::chrono::steady_clock::time_point{};
            scanEndTime = std::chrono::steady_clock::time_point{};

            state = ScannerState::SCANNING;
            justReset = true;
        }

        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        glDeleteTextures(1, &tex);

        int key = cv::waitKey(1) & 0xFF;
        if (key == 'q')
            break;
        if (key == 'c' || key == 'C') {
            seen.clear();
            scanIndex = 0;
            timingStarted = false;
            timingStopped = false;
            scanStartTime = std::chrono::steady_clock::time_point{};
            scanEndTime = std::chrono::steady_clock::time_point{};
            scannedCodes.clear();
            state = ScannerState::SCANNING;
            justReset = true;
            std::cout << "🔄 Scan reset — ready for new box\n";
        }
        if (key >= '0' && key <= '9') {
            int newIndex = key - '0';
            if (newIndex != cameraIndex) {
                std::cout << "🎥 Switching camera to index " << newIndex << std::endl;
                cap.release();
                cameraIndex = newIndex;
                cap.open(cameraIndex, cv::CAP_AVFOUNDATION);
                if (!cap.isOpened()) {
                    std::cerr << "❌ Cannot open camera " << cameraIndex << std::endl;
                }
            }
        }
        if (key == '+' || key == '=') {
            zoom = std::min(zoom + zoomStep, zoomMax);
            std::cout << "🔍 Zoom: " << zoom << "x\n";
        }
        if (key == '-' || key == '_') {
            zoom = std::max(zoom - zoomStep, zoomMin);
            std::cout << "🔍 Zoom: " << zoom << "x\n";
        }

        justReset = false;

        glfwPollEvents();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    cap.release();
    cv::destroyAllWindows();
    return 0;
}