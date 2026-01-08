#define GL_SILENCE_DEPRECATION
#include <string>
#include <opencv2/opencv.hpp>
// --- GS1 DataMatrix normalization ---
// Приводит все варианты разделителей к ASCII 29 (\x1D)
inline std::string normalizeGS1(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());

    for (size_t i = 0; i < raw.size(); ++i) {
        // Реальный ASCII GS (0x1D)
        if (raw[i] == '\x1D') {
            out.push_back('\x1D');
            continue;
        }

        // Последовательность "<GS>"
        if (raw[i] == '<' && i + 3 < raw.size()
            && raw[i + 1] == 'G'
            && raw[i + 2] == 'S'
            && raw[i + 3] == '>') {
            out.push_back('\x1D');
            i += 3;
            continue;
        }

        // ВСЁ ОСТАЛЬНОЕ — копируем как есть
        out.push_back(raw[i]);
    }

    return out;
}

// --- GS1 printable helper ---
inline std::string printableGS1(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char ch : s) {
        if (ch == 0x1D) out += "<GS>";
        else out.push_back(ch);
    }
    return out;
}
// --- ROI bounds check helper ---
inline bool isValidROI(const cv::Rect& r, const cv::Mat& m)
{
    return r.width > 0 && r.height > 0 &&
           r.x >= 0 && r.y >= 0 &&
           r.x + r.width <= m.cols &&
           r.y + r.height <= m.rows;
}
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

enum class AggregationResult {
    NONE,
    OK,
    ALREADY_EXISTS,
    NO_ORDER,
    VALIDATION_ERROR,
    ERROR
};

enum class SendMode {
    AUTO,
    MANUAL
};

bool testMode = true; // TEST / PROD

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
AggregationResult sendAggregation(
    const std::vector<std::string>& codes,
    std::string& out_sscc,
    int& out_order_id
)
{
    using json = nlohmann::json;

    httplib::Client cli("http://127.0.0.1:8000");
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

    json payload;
    payload["device_id"] = "table_01";
    payload["codes"] = codes;

    std::string endpoint = testMode
        ? "/api/v1/camera/scan/aggregation/test"
        : "/api/v1/camera/scan/aggregation";

    auto res = cli.Post(endpoint.c_str(), payload.dump(), "application/json");

    out_sscc.clear();
    out_order_id = -1;

    if (!res)
        return AggregationResult::ERROR;

    auto response = json::parse(res->body, nullptr, false);
    if (response.is_discarded())
        return AggregationResult::ERROR;

    std::string status = response.value("status", "");

    out_sscc = response.value("sscc_code", "");
    out_order_id = response.value("order_id", -1);

    if (status == "OK")
        return AggregationResult::OK;
    if (status == "ALREADY_EXISTS")
        return AggregationResult::ALREADY_EXISTS;
    if (status == "NO_ORDER")
        return AggregationResult::NO_ORDER;
    if (status == "VALIDATION_ERROR")
        return AggregationResult::VALIDATION_ERROR;

    return AggregationResult::ERROR;
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

cv::Rect makeSquareROI(const cv::Rect& r, const cv::Size& bounds, float padding = 0.3f)
{
    int size = std::max(r.width, r.height);
    int pad = static_cast<int>(size * padding);
    size += pad * 2;

    int cx = r.x + r.width / 2;
    int cy = r.y + r.height / 2;

    int x = std::max(0, cx - size / 2);
    int y = std::max(0, cy - size / 2);

    if (x + size > bounds.width)  size = bounds.width - x;
    if (y + size > bounds.height) size = bounds.height - y;

    return cv::Rect(x, y, size, size);
}

// --- dev/test: reset TEST_AGGREGATIONS on backend ---
bool sendTestReset()
{
    httplib::Client cli("http://127.0.0.1:8000");
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);

    auto res = cli.Post("/api/v1/camera/scan/aggregation/test/reset");

    if (!res || res->status != 200) {
        std::cerr << "❌ Failed to reset TEST_AGGREGATIONS\n";
        return false;
    }

    return true;
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

    ImGuiStyle& style = ImGui::GetStyle();

    // Rounded corners (Cursor / modern UI feel)
    style.WindowRounding = 12.0f;      // main windows
    style.ChildRounding  = 10.0f;      // panels / cards
    style.FrameRounding  = 8.0f;       // buttons, inputs
    style.PopupRounding  = 10.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 8.0f;

    // Softer window borders
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize  = 0.0f;

    // Slightly more padding (cleaner layout)
    style.WindowPadding = ImVec2(16, 14);
    style.FramePadding  = ImVec2(10, 8);
    style.ItemSpacing   = ImVec2(10, 10);

    ImVec4 C_ACCENT  = ImVec4(0.20f, 0.55f, 0.90f, 1.00f);
    ImVec4 C_SUCCESS = ImVec4(0.25f, 0.75f, 0.45f, 1.00f);
    ImVec4 C_WARN    = ImVec4(0.95f, 0.75f, 0.20f, 1.00f);
    ImVec4 C_ERROR   = ImVec4(0.90f, 0.30f, 0.30f, 1.00f);
    ImVec4 C_DIM     = ImVec4(0.10f, 0.10f, 0.10f, 0.65f);

    const int GRID_THICKNESS = 1;
    const double GRID_ALPHA = 0.4;   // transparency (0.3–0.5 recommended)

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 120");

int cameraIndex = 0;
    // Camera selector (simple indexed list)
    const int MAX_CAMERAS = 2; // adjust if needed
    int selectedCameraIndex = cameraIndex;

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
    auto lastActivityTime = std::chrono::steady_clock::now();
    bool idleMode = false;

    bool timingStarted = false;
    auto scanStartTime = std::chrono::steady_clock::time_point{};
    bool timingStopped = false;
    auto scanEndTime = std::chrono::steady_clock::time_point{};

    // --- Коробочные метрики ---
    float lastBoxScanTimeSec = 0.0f;
    int lastBoxCodesCount = 0;

    std::vector<std::string> scannedCodes;
    ScannerState state = ScannerState::SCANNING;
    bool justReset = false;

    // --- UI state ---
    SendMode sendMode = SendMode::AUTO;
    AggregationResult lastAggResult = AggregationResult::NONE;
    std::string ui_sscc;
    int ui_order_id = -1;
    int boxCounter = 0;
    bool adaptiveMode = false;
    cv::Rect lastAdaptiveROI;

    double zoom = 1.0;          // 1.0 = no zoom
    const double zoomStep = 0.1;
    const double zoomMin = 1.0;
    const double zoomMax = 2.0;

    // --- fixed box & grid configuration ---
    const int GRID_ROWS = 2;
    const int GRID_COLS = 5;

    // Per-cell state for blinking red logic
    std::vector<std::chrono::steady_clock::time_point> cellStartTime(
        GRID_ROWS * GRID_COLS,
        std::chrono::steady_clock::now()
    );
    std::vector<bool> cellResolved(GRID_ROWS * GRID_COLS, false);

    // Central box ROI as % of frame (tuned for fixed corner guides)
    const double BOX_W = 0.80;   // 80% width
    const double BOX_H = 0.90;   // 70% height

    const double CELL_ZOOM = 2.0; // aggressive local zoom per cell

    // --- CLAHE and preview brightness parameters ---
    float claheClipLimit = 3.5f; // UI-controlled brightness/contrast (brighter default)
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(claheClipLimit, cv::Size(8, 8));

    // Preview brightness (visual only)
    double previewAlpha = 1.0; // contrast
    double previewBeta  = 0.0; // brightness

    while (!glfwWindowShouldClose(window)) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty())
            break;
        if (idleMode && state == ScannerState::SCANNING) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // --- Preview brightness mapping (visual feedback) ---
        previewAlpha = 1.05 + (claheClipLimit - 3.0) * 0.25;
        previewBeta  = 20.0 + (claheClipLimit - 3.0) * 30.0;

        frame.convertTo(frame, -1, previewAlpha, previewBeta);

        // --- central BOX ROI ---
        int bw = static_cast<int>(frame.cols * BOX_W);
        int bh = static_cast<int>(frame.rows * BOX_H);
        int bx = (frame.cols - bw) / 2;
        int by = (frame.rows - bh) / 2;
        cv::Rect boxROI(bx, by, bw, bh);

        // Darken outside ROI (focus mask)
        cv::Mat overlay;
        frame.copyTo(overlay);
        cv::rectangle(overlay, cv::Rect(0, 0, frame.cols, frame.rows), cv::Scalar(0,0,0), -1);
        cv::rectangle(overlay, boxROI, cv::Scalar(0,0,0), -1);
        cv::addWeighted(overlay, 0.55, frame, 0.45, 0, frame);

        // draw box ROI
        cv::rectangle(frame, boxROI, cv::Scalar(255, 255, 0), 2);

        int cellW = bw / GRID_COLS;
        int cellH = bh / GRID_ROWS;


        // --- Adaptive DM pass (preferred) ---
        if (adaptiveMode && state == ScannerState::SCANNING && !justReset && !idleMode) {
            if (scanIndex >= 10)
                break;

            if (!isValidROI(lastAdaptiveROI, frame)) {
                adaptiveMode = false;   // fail-safe
            } else {
                cv::Mat roi = frame(lastAdaptiveROI).clone();

                cv::Mat gray;
                cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);

                ZXing::ImageView image(gray.data, gray.cols, gray.rows, ZXing::ImageFormat::Lum);
                auto barcodes = ZXing::ReadBarcodes(image, hints);

                for (const auto& barcode : barcodes) {
                    if (!barcode.isValid() || barcode.text().empty())
                        continue;

                    // Always normalize GS1
                    std::string normalized = normalizeGS1(barcode.text());
                    if (seen.count(normalized))
                        continue;

                    if (!timingStarted) {
                        timingStarted = true;
                        scanStartTime = std::chrono::steady_clock::now();
                    }

                    seen.insert(normalized);
                    scannedCodes.push_back(normalized);
                    ++scanIndex;
                    lastActivityTime = std::chrono::steady_clock::now();
                    idleMode = false;

                    auto pos = barcode.position();
                    cv::Rect localRect(
                        static_cast<int>(pos.topLeft().x),
                        static_cast<int>(pos.topLeft().y),
                        static_cast<int>(pos.bottomRight().x - pos.topLeft().x),
                        static_cast<int>(pos.bottomRight().y - pos.topLeft().y)
                    );

                    // clamp to frame bounds (ZXing may give out-of-range coords)
                    localRect &= cv::Rect(0, 0, frame.cols, frame.rows);

                    localRect.x += lastAdaptiveROI.x;
                    localRect.y += lastAdaptiveROI.y;

                    cv::Rect candidateROI = makeSquareROI(localRect, frame.size());
                    if (isValidROI(candidateROI, frame)) {
                        lastAdaptiveROI = candidateROI;
                    } else {
                        adaptiveMode = false; // invalid ROI → fallback to grid
                    }

                    cv::rectangle(frame, lastAdaptiveROI, cv::Scalar(0, 200, 255), 3);

                    if (scanIndex == 10 && !timingStopped) {
                        timingStopped = true;
                        scanEndTime = std::chrono::steady_clock::now();
                        state = ScannerState::READY;
                    }

                    std::cout << scanIndex << ". " << printableGS1(normalized) << std::endl;
                    std::cout << "\a" << std::flush;

                    break; // one per frame
                }
            }
        }
        for (int r = 0; r < GRID_ROWS; ++r) {
            for (int c = 0; c < GRID_COLS; ++c) {
                // Scan ONLY in active SCANNING state
                if (state != ScannerState::SCANNING || justReset || idleMode)
                    continue;

                int idx = r * GRID_COLS + c;
                // Do not spend CPU on already successful cells
                if (cellResolved[idx])
                    continue;

                int cx = bx + c * cellW;
                int cy = by + r * cellH;
                cv::Rect cellROI(cx, cy, cellW, cellH);

                // --- ЯВНАЯ ТАБЛИЦА ЦВЕТОВ ---
                cv::Scalar gridColor(255, 255, 255); // IDLE = white
                int thickness = GRID_THICKNESS;

                auto now = std::chrono::steady_clock::now();

                // Защита: не запускать SCANNING для success
                if (cellResolved[idx])
                    continue;

                double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - cellStartTime[idx]
                ).count() / 1000.0;

                if (elapsed < 1.0) {
                    // SCANNING → blinking green
                    double t = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()
                    ).count() / 1000.0;

                    if (std::sin(t * 6.2831853) > 0) {
                        gridColor = cv::Scalar(0, 255, 0);
                        thickness = 3;
                    }
                }
                else if (elapsed >= 1.0) {
                    // FAILED → solid red
                    gridColor = cv::Scalar(0, 0, 255);
                    thickness = 3;
                }

                // РИСОВАНИЕ СЕТКИ — ТОЛЬКО ОДИН РАЗ
                cv::rectangle(
                    frame,
                    cellROI,
                    gridColor,
                    thickness,
                    cv::LINE_AA
                );

                // skip already scanned cells by content (global dedup still applies)
                if (!isValidROI(cellROI, frame))
                    continue;
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
                    std::string normalized = normalizeGS1(text);

                    // HARD STOP: never scan more than 10
                    if (scanIndex >= 10)
                        break;

                    if (seen.find(normalized) != seen.end())
                        continue;

                    if (!timingStarted) {
                        timingStarted = true;
                        scanStartTime = std::chrono::steady_clock::now();
                    }

                    seen.insert(normalized);
                    scannedCodes.push_back(normalized);
                    ++scanIndex;
                    lastActivityTime = std::chrono::steady_clock::now();
                    idleMode = false;
                    if (!adaptiveMode) {
                        adaptiveMode = true;
                        cv::Rect candidateROI = makeSquareROI(cellROI, frame.size());
                        if (isValidROI(candidateROI, frame)) {
                            lastAdaptiveROI = candidateROI;
                        } else {
                            adaptiveMode = false; // invalid ROI → fallback to grid
                        }
                    }
                    if (state == ScannerState::SCANNING && scanIndex == 10 && !timingStopped) {
                        timingStopped = true;
                        scanEndTime = std::chrono::steady_clock::now();

                        state = ScannerState::READY;
                        std::cout << "🟢 State → READY (10/10 scanned)\n";
                    }
                    std::cout << scanIndex << ". " << printableGS1(normalized) << std::endl;
                    std::cout << "\a" << std::flush;

                    // mark cell as resolved
                    cellResolved[r * GRID_COLS + c] = true;
                }
            }
        }


        // --- overlay: scanned count ---
        // removed OpenCV text overlays

        // --- Idle check (throttling) ---
        auto nowIdleCheck = std::chrono::steady_clock::now();
        double idleSec = std::chrono::duration_cast<std::chrono::milliseconds>(
            nowIdleCheck - lastActivityTime
        ).count() / 1000.0;

        idleMode = (idleSec > 0.6); // 600 мс без кодов = AFK

        // --- State machine: handle READY state ---
        if (state == ScannerState::READY && sendMode == SendMode::AUTO) {
            state = ScannerState::SENDING;
        }

        // --- State machine: handle SENDING state ---
        if (state == ScannerState::SENDING) {
            AggregationResult res = sendAggregation(scannedCodes, ui_sscc, ui_order_id);
            lastAggResult = res;

            // сохранить метрики текущей коробки
            if (timingStarted) {
                auto endTime = timingStopped ? scanEndTime : std::chrono::steady_clock::now();
                lastBoxScanTimeSec =
                    std::chrono::duration_cast<std::chrono::milliseconds>(endTime - scanStartTime).count() / 1000.0f;
            } else {
                lastBoxScanTimeSec = 0.0f;
            }
            lastBoxCodesCount = static_cast<int>(scannedCodes.size());

            if (res == AggregationResult::OK) {
                boxCounter++;
                state = ScannerState::DONE;
            } else if (res == AggregationResult::ALREADY_EXISTS) {
                state = ScannerState::DONE;
            } else {
                state = ScannerState::ERROR;
            }

            // очистка сканера после ответа
            seen.clear();
            scannedCodes.clear();
            scanIndex = 0;
            timingStarted = timingStopped = false;
        }

        // NOTE: do NOT clear 'seen' automatically; scanned codes must not be rescanned

        // Автоматический запуск новой коробки после отправки
        if (state == ScannerState::DONE && lastBoxCodesCount == 10) {
            state = ScannerState::SCANNING;
            justReset = true;
            adaptiveMode = false;
            std::fill(cellResolved.begin(), cellResolved.end(), false);
            auto now = std::chrono::steady_clock::now();
            std::fill(cellStartTime.begin(), cellStartTime.end(), now);
            ui_sscc.clear();
            ui_order_id = -1;
            lastAggResult = AggregationResult::NONE;
            lastBoxScanTimeSec = 0.0f;
            lastBoxCodesCount = 0;
            std::cout << "🔄 Автоматический запуск новой коробки\n";
        }

        GLuint tex = matToTexture(frame);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Camera window
        ImGui::Begin("Камера", nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar
        );
        // Preserve aspect ratio
        float availWidth = ImGui::GetContentRegionAvail().x;
        float aspect = (float)frame.rows / (float)frame.cols;
        ImVec2 imageSize(availWidth, availWidth * aspect);
        ImGui::Image((void*)(intptr_t)tex, imageSize);

        // --- нижняя панель статистики коробки ---
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
        ImGui::Text("Текущая коробка");
        ImGui::PopStyleColor();

        if (lastBoxCodesCount > 0) {
            ImGui::Text("Кодов отсканировано: %d / 10", lastBoxCodesCount);
            ImGui::Text("Время сканирования: %.2f сек", lastBoxScanTimeSec);
        } else {
            ImGui::Text("Ожидание начала сканирования…");
        }

        ImGui::End();

        // Info panel
        ImGui::Begin("Панель сканирования", nullptr,
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar
        );

        // STATUS (Russian UI)
        ImVec4 statusColor = C_ACCENT;
        const char* uiStatus =
            state == ScannerState::SCANNING ?
                (idleMode ? "ОЖИДАНИЕ КОДОВ" : "СКАНИРОВАНИЕ") :
            state == ScannerState::READY    ? "ГОТОВО К ОТПРАВКЕ" :
            state == ScannerState::SENDING  ? "ОБРАБОТКА" :
            state == ScannerState::DONE ?
                (lastAggResult == AggregationResult::OK ? "АГРЕГАЦИЯ ВЫПОЛНЕНА" :
                 lastAggResult == AggregationResult::ALREADY_EXISTS ? "АГРЕГАТ УЖЕ БЫЛ НАПОЛНЕН" :
                 lastAggResult == AggregationResult::NO_ORDER ? "НЕТ ЗАКАЗА С ЭТИМИ КОДАМИ" :
                 lastAggResult == AggregationResult::VALIDATION_ERROR ? "ОШИБКА ВАЛИДАЦИИ" :
                 "ОШИБКА")
            : "ОШИБКА";
        if (state == ScannerState::DONE) statusColor = C_SUCCESS;
        else if (state == ScannerState::READY || state == ScannerState::SENDING) statusColor = C_WARN;
        else if (state == ScannerState::ERROR) statusColor = C_ERROR;

        ImGui::PushStyleColor(ImGuiCol_Text, statusColor);
        ImGui::Text("● %s", uiStatus);
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // PROGRESS (main)
        float progress = scanIndex / 10.0f;
        ImGui::ProgressBar(progress, ImVec2(-1, 26));
        ImGui::Text("Коды: %zu / 10", scanIndex);

        // TIMER (badge)
        if (timingStarted) {
            auto endTime = timingStopped ? scanEndTime : std::chrono::steady_clock::now();
            float sec = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - scanStartTime).count() / 1000.0f;

            ImVec4 tcol = sec < 3.0 ? C_SUCCESS : (sec < 5.0 ? C_WARN : C_ERROR);
            ImGui::PushStyleColor(ImGuiCol_Text, tcol);
            ImGui::Text("⏱ %.2f сек", sec);
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
        ImGui::Text("Камера");
        ImGui::PopStyleColor();

        // Build camera labels dynamically: "Camera 0", "Camera 1", ...
        static const char* cameraLabels[MAX_CAMERAS];
        static std::string cameraLabelStorage[MAX_CAMERAS];
        for (int i = 0; i < MAX_CAMERAS; ++i) {
            cameraLabelStorage[i] = "Камера " + std::to_string(i);
            cameraLabels[i] = cameraLabelStorage[i].c_str();
        }

        if (ImGui::Combo("##camera_select", &selectedCameraIndex, cameraLabels, MAX_CAMERAS)) {
            if (selectedCameraIndex != cameraIndex) {
                std::cout << "🎥 Switching camera to index " << selectedCameraIndex << std::endl;
                cap.release();
                cameraIndex = selectedCameraIndex;
                cap.open(cameraIndex, cv::CAP_AVFOUNDATION);
                if (!cap.isOpened()) {
                    std::cerr << "❌ Cannot open camera " << cameraIndex << std::endl;
                }
            }
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, C_DIM);
        ImGui::Text("Яркость");
        ImGui::PopStyleColor();
        ImGui::SliderFloat(
            "##brightness",
            &claheClipLimit,
            1.0f,
            6.0f,
            "%.1f"
        );

        ImGui::Separator();

        // Info Panel: SSCC, Order ID, Box Counter
        if (!ui_sscc.empty())
            ImGui::Text("SSCC: %s", ui_sscc.c_str());
        if (ui_order_id > 0)
            ImGui::Text("Заказ: %d", ui_order_id);
        ImGui::Text("Коробок отсканировано: %d", boxCounter);

        ImGui::Spacing();

        ImGui::Separator();

        // TEST mode toggle
        ImGui::Checkbox("ТЕСТ режим", &testMode);

        // Управление режимом отправки
        ImGui::Text("Режим отправки");
        ImGui::RadioButton("Авто", (int*)&sendMode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("По кнопке", (int*)&sendMode, 1);

        // Кнопка "Отправить" (MANUAL + READY)
        if (sendMode == SendMode::MANUAL && state == ScannerState::READY) {
            if (ImGui::Button("Отправить", ImVec2(-1, 40))) {
                state = ScannerState::SENDING;
            }
        }

        // ACTION
        if (state == ScannerState::SENDING)
            ImGui::BeginDisabled();

        if (ImGui::Button("Попробовать еще раз", ImVec2(-1, 42))) {
            seen.clear();
            scanIndex = 0;
            scannedCodes.clear();
            timingStarted = timingStopped = false;
            scanStartTime = scanEndTime = {};
            state = ScannerState::SCANNING;
            justReset = true;
            adaptiveMode = false;
            std::fill(cellResolved.begin(), cellResolved.end(), false);
            auto now = std::chrono::steady_clock::now();
            std::fill(cellStartTime.begin(), cellStartTime.end(), now);
            ui_sscc.clear();
            ui_order_id = -1;
            lastAggResult = AggregationResult::NONE;
            lastBoxScanTimeSec = 0.0f;
            lastBoxCodesCount = 0;
            std::cout << "🔄 Попробовать еще раз — сброс состояния\n";
        }

        if (state == ScannerState::SENDING)
            ImGui::EndDisabled();

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
            adaptiveMode = false;
            std::fill(cellResolved.begin(), cellResolved.end(), false);
            auto now = std::chrono::steady_clock::now();
            std::fill(cellStartTime.begin(), cellStartTime.end(), now);
            lastBoxScanTimeSec = 0.0f;
            lastBoxCodesCount = 0;
            std::cout << "🔄 Scan reset — ready for new box\n";
        }
        if (key >= '0' && key <= '9') {
            int newIndex = key - '0';
            if (newIndex != cameraIndex) {
                std::cout << "🎥 Switching camera to index " << newIndex << std::endl;
                cap.release();
                cameraIndex = newIndex;
                selectedCameraIndex = cameraIndex;
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