#define GL_SILENCE_DEPRECATION
#include <string>
#include <vector>
#include <map>
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

enum class AggregationResult {
    NONE,
    OK,
    ALREADY_EXISTS,
    ERROR
};

enum class SendMode {
    AUTO,
    MANUAL
};

struct OrderInfo {
    std::string mode; // "TEST" | "PROD"
    std::string order_name;
    std::string product_name;
    std::string batch_number;
    std::string prod_date;
    std::string exp_date;
    int total_codes = 0;
};

struct BoxQueueItem {
    int box_id;
    std::string sscc;
    int order_id;
    ScannerState state;
};

std::vector<BoxQueueItem> boxQueue;

const char* stateToStr(ScannerState s)
{
    switch (s) {
        case ScannerState::SCANNING: return "Сканирование";
        case ScannerState::READY:    return "Готово";
        case ScannerState::SENDING:  return "Отправка";
        case ScannerState::DONE:     return "Завершено";
        case ScannerState::ERROR:    return "Ошибка";
        default: return "UNKNOWN";
    }
}

// Send aggregation request to FastAPI server
AggregationResult sendAggregation(
    const std::vector<std::string>& codes,
    std::string& out_sscc,
    int& out_order_id,
    bool testMode
)
{
    using json = nlohmann::json;

    httplib::Client cli("http://127.0.0.1:8000");
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

    json payload;
    payload["device_id"] = "table_01";
    payload["codes"] = codes;

    const char* url = testMode
        ? "/api/v1/camera/scan/aggregation/test"
        : "/api/v1/camera/scan/aggregation";

    auto res = cli.Post(
        url,
        payload.dump(),
        "application/json"
    );

    out_sscc.clear();
    out_order_id = -1;

    if (!res) {
        return AggregationResult::ERROR;
    }

    if (res->status != 200) {
        return AggregationResult::ERROR;
    }

    auto response = json::parse(res->body);
    std::string status = response.value("status", "");
    out_sscc = response.value("sscc_code", "");
    out_order_id = response.value("order_id", -1);
    (void)response.value("success", false);

    if (status == "OK")
        return AggregationResult::OK;
    else if (status == "ALREADY_EXISTS")
        return AggregationResult::ALREADY_EXISTS;
    else
        return AggregationResult::ERROR;
}

bool fetchOrderInfo(const std::vector<std::string>& codes, OrderInfo& outInfo)
{
    using json = nlohmann::json;

    httplib::Client cli("http://127.0.0.1:8000");
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

    json payload;
    payload["device_id"] = "table_01";
    payload["codes"] = codes;

    auto res = cli.Post(
        "/api/v1/camera/order-info",
        payload.dump(),
        "application/json"
    );

    if (!res || res->status != 200)
        return false;

    auto j = json::parse(res->body, nullptr, false);
    if (j.is_discarded())
        return false;

    outInfo.mode         = j.value("mode", "");
    outInfo.order_name   = j.value("order_name", "");
    outInfo.product_name = j.value("product_name", "");
    outInfo.batch_number = j.value("batch_number", "");
    outInfo.prod_date    = j.value("prod_date", "");
    outInfo.exp_date     = j.value("exp_date", "");
    outInfo.total_codes  = j.value("total_codes", 0);

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

std::string normalizeGS1(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());

    for (unsigned char ch : raw) {
        if (ch == 29) {
            out.push_back(ch);
            continue;
        }

        if (ch < 32 || ch == 127)
            continue;

        out.push_back(ch);
    }

    return out;
}

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

// --- Apple macOS Dark Mode Style ---
void ApplyAppleDarkModeStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();

    // Rounded corners (Apple-style)
    style.WindowRounding    = 10.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;

    // Spacing (Apple-like proportions)
    style.WindowPadding     = ImVec2(12, 12);
    style.FramePadding      = ImVec2(12, 6);
    style.ItemSpacing       = ImVec2(8, 6);
    style.ItemInnerSpacing  = ImVec2(4, 4);
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 10.0f;

    // Borders
    style.WindowBorderSize  = 0.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 0.0f;

    // Colors (macOS Dark Mode)
    ImVec4* colors = style.Colors;
    
    // Base colors
    colors[ImGuiCol_Text]                   = ImVec4(0.96f, 0.96f, 0.96f, 1.00f);
    colors[ImGuiCol_TextDisabled]           = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    
    // Window/background
    colors[ImGuiCol_WindowBg]               = ImVec4(0.11f, 0.11f, 0.11f, 0.94f);
    colors[ImGuiCol_ChildBg]                = ImVec4(0.16f, 0.16f, 0.16f, 0.60f);
    colors[ImGuiCol_PopupBg]                = ImVec4(0.08f, 0.08f, 0.08f, 0.94f);
    
    // Borders
    colors[ImGuiCol_Border]                 = ImVec4(0.27f, 0.27f, 0.27f, 0.50f);
    colors[ImGuiCol_BorderShadow]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    
    // Frame background
    colors[ImGuiCol_FrameBg]                = ImVec4(0.20f, 0.20f, 0.20f, 0.54f);
    colors[ImGuiCol_FrameBgHovered]         = ImVec4(0.28f, 0.28f, 0.28f, 0.54f);
    colors[ImGuiCol_FrameBgActive]          = ImVec4(0.32f, 0.32f, 0.32f, 0.54f);
    
    // Title
    colors[ImGuiCol_TitleBg]                = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]          = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.10f, 0.10f, 0.10f, 0.51f);
    
    // Menu bar
    colors[ImGuiCol_MenuBarBg]              = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    
    // Scrollbar
    colors[ImGuiCol_ScrollbarBg]            = ImVec4(0.02f, 0.02f, 0.02f, 0.53f);
    colors[ImGuiCol_ScrollbarGrab]          = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.41f, 0.41f, 0.41f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]    = ImVec4(0.51f, 0.51f, 0.51f, 1.00f);
    
    // Checkbox
    colors[ImGuiCol_CheckMark]              = ImVec4(0.10f, 0.64f, 1.00f, 1.00f);
    
    // Slider
    colors[ImGuiCol_SliderGrab]             = ImVec4(0.10f, 0.64f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]       = ImVec4(0.08f, 0.56f, 0.95f, 1.00f);
    
    // Buttons
    colors[ImGuiCol_Button]                 = ImVec4(0.27f, 0.27f, 0.27f, 0.40f);
    colors[ImGuiCol_ButtonHovered]          = ImVec4(0.34f, 0.34f, 0.34f, 1.00f);
    colors[ImGuiCol_ButtonActive]           = ImVec4(0.10f, 0.64f, 1.00f, 1.00f);
    
    // Headers
    colors[ImGuiCol_Header]                 = ImVec4(0.10f, 0.64f, 1.00f, 0.31f);
    colors[ImGuiCol_HeaderHovered]          = ImVec4(0.10f, 0.64f, 1.00f, 0.80f);
    colors[ImGuiCol_HeaderActive]           = ImVec4(0.10f, 0.64f, 1.00f, 1.00f);
    
    // Separator
    colors[ImGuiCol_Separator]              = ImVec4(0.27f, 0.27f, 0.27f, 0.50f);
    colors[ImGuiCol_SeparatorHovered]       = ImVec4(0.10f, 0.40f, 0.75f, 0.78f);
    colors[ImGuiCol_SeparatorActive]        = ImVec4(0.10f, 0.40f, 0.75f, 1.00f);
    
    // Resize grip
    colors[ImGuiCol_ResizeGrip]             = ImVec4(0.10f, 0.64f, 1.00f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]      = ImVec4(0.10f, 0.64f, 1.00f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]       = ImVec4(0.10f, 0.64f, 1.00f, 0.95f);
    
    // Tabs
    colors[ImGuiCol_Tab]                    = ImVec4(0.18f, 0.18f, 0.18f, 0.86f);
    colors[ImGuiCol_TabHovered]             = ImVec4(0.26f, 0.26f, 0.26f, 0.80f);
    colors[ImGuiCol_TabActive]              = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    colors[ImGuiCol_TabUnfocused]           = ImVec4(0.18f, 0.18f, 0.18f, 0.98f);
    colors[ImGuiCol_TabUnfocusedActive]     = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    
    // Plot
    colors[ImGuiCol_PlotLines]              = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]       = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    colors[ImGuiCol_PlotHistogram]          = ImVec4(0.10f, 0.64f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]   = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    
    // Table
    colors[ImGuiCol_TableHeaderBg]          = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]      = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);
    colors[ImGuiCol_TableBorderLight]       = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);
    colors[ImGuiCol_TableRowBg]             = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt]          = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    
    // Text selection
    colors[ImGuiCol_TextSelectedBg]         = ImVec4(0.10f, 0.64f, 1.00f, 0.35f);
    
    // Drag and drop
    colors[ImGuiCol_DragDropTarget]         = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    
    // Navigation
    colors[ImGuiCol_NavHighlight]           = ImVec4(0.10f, 0.64f, 1.00f, 1.00f);
    colors[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    
    // Modal window dim
    colors[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
}

int main()
{
    // --- GLFW initialization ---
    if (!glfwInit()) {
        std::cerr << "❌ GLFW init failed\n";
        return 1;
    }

    GLFWwindow* window = glfwCreateWindow(1800, 1000, "GRUNDLAGE", nullptr, nullptr);
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
    
    // Load SF Pro font on macOS
    io.Fonts->AddFontFromFileTTF(
        "/System/Library/Fonts/SFNS.ttf",
        15.0f,
        nullptr,
        io.Fonts->GetGlyphRangesCyrillic()
    );
    
    // Small font for captions
    ImFont* smallFont = io.Fonts->AddFontFromFileTTF(
        "/System/Library/Fonts/SFNS.ttf",
        13.0f,
        nullptr,
        io.Fonts->GetGlyphRangesCyrillic()
    );
    
    // Bold font for headings
    ImFont* headingFont = io.Fonts->AddFontFromFileTTF(
        "/System/Library/Fonts/SFNS.ttf",
        17.0f,
        nullptr,
        io.Fonts->GetGlyphRangesCyrillic()
    );
    
    io.FontDefault = io.Fonts->Fonts[0];

    // Apply Apple dark mode style
    ApplyAppleDarkModeStyle();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 120");

    int cameraIndex = 0;
    const int MAX_CAMERAS = 2;
    int selectedCameraIndex = cameraIndex;

    cv::VideoCapture cap(cameraIndex, cv::CAP_AVFOUNDATION);
    if (!cap.isOpened()) {
        std::cerr << "❌ Cannot open camera\n";
        return 1;
    }

    std::cout << "▶ Codex Scanner started (Q to quit)\n";

    ZXing::ReaderOptions hints;
    hints.setFormats(ZXing::BarcodeFormat::DataMatrix);
    hints.setTryHarder(true);

    std::unordered_set<std::string> seen;
    size_t scanIndex = 0;

    bool timingStarted = false;
    auto scanStartTime = std::chrono::steady_clock::time_point{};
    bool timingStopped = false;
    auto scanEndTime = std::chrono::steady_clock::time_point{};

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
    int targetBoxes = 10;
    bool adaptiveMode = false;
    cv::Rect lastAdaptiveROI;

    bool orderCheckMode = false;
    OrderInfo orderInfo;
    bool orderInfoLoaded = false;
    bool testMode = false;

    // --- Target codes configuration ---
    int targetCodes = 10;
    bool gridEnabled = true;
    bool showGridOverlay = true; // Subtle guide lines

    // fixed box & grid configuration
    const int GRID_ROWS = 2;
    const int GRID_COLS = 5;

    std::vector<std::chrono::steady_clock::time_point> cellStartTime(
        GRID_ROWS * GRID_COLS,
        std::chrono::steady_clock::now()
    );
    std::vector<bool> cellResolved(GRID_ROWS * GRID_COLS, false);

    const double BOX_W = 0.80;
    const double BOX_H = 0.90;
    const double CELL_ZOOM = 2.0;

    // --- CLAHE and preview brightness parameters ---
    float claheClipLimit = 3.5f;
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(claheClipLimit, cv::Size(8, 8));

    double previewAlpha = 1.0;
    double previewBeta  = 0.0;

    // Colors for status indicators (muted, professional)
    ImVec4 C_SUCCESS = ImVec4(0.20f, 0.85f, 0.30f, 1.00f);  // Muted green
    ImVec4 C_WARNING = ImVec4(1.00f, 0.75f, 0.00f, 1.00f);  // Amber
    ImVec4 C_ERROR = ImVec4(1.00f, 0.35f, 0.35f, 1.00f);    // Soft red
    ImVec4 C_ACCENT = ImVec4(0.10f, 0.64f, 1.00f, 1.00f);   // Apple blue
    ImVec4 C_DIM = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);      // Dim text
    ImVec4 C_DARK_BG = ImVec4(0.08f, 0.08f, 0.08f, 0.90f);  // Dark overlay

    while (!glfwWindowShouldClose(window)) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty())
            break;

        previewAlpha = 1.05 + (claheClipLimit - 3.0) * 0.25;
        previewBeta  = 20.0 + (claheClipLimit - 3.0) * 30.0;

        frame.convertTo(frame, -1, previewAlpha, previewBeta);

        // Draw dark overlay outside scanning area
        if (gridEnabled) {
            int bw = static_cast<int>(frame.cols * BOX_W);
            int bh = static_cast<int>(frame.rows * BOX_H);
            int bx = (frame.cols - bw) / 2;
            int by = (frame.rows - bh) / 2;
            cv::Rect boxROI(bx, by, bw, bh);

            // Darken outside ROI with subtle vignette
            cv::Mat overlay;
            frame.copyTo(overlay);
            cv::rectangle(overlay, cv::Rect(0, 0, frame.cols, frame.rows), cv::Scalar(0,0,0), -1);
            cv::rectangle(overlay, boxROI, cv::Scalar(0,0,0), -1);
            cv::addWeighted(overlay, 0.7, frame, 0.3, 0, frame);

            // Draw subtle focus frame (professional, not flashy)
            cv::rectangle(frame, boxROI, cv::Scalar(180, 180, 180), 1, cv::LINE_AA);

            // Draw very subtle grid lines if enabled
            if (showGridOverlay) {
                int cellW = bw / GRID_COLS;
                int cellH = bh / GRID_ROWS;
                
                for (int r = 1; r < GRID_ROWS; ++r) {
                    int y = by + r * cellH;
                    cv::line(frame, cv::Point(bx, y), cv::Point(bx + bw, y), 
                            cv::Scalar(100, 100, 100, 0.3), 1, cv::LINE_AA);
                }
                
                for (int c = 1; c < GRID_COLS; ++c) {
                    int x = bx + c * cellW;
                    cv::line(frame, cv::Point(x, by), cv::Point(x, by + bh), 
                            cv::Scalar(100, 100, 100, 0.3), 1, cv::LINE_AA);
                }
            }
        }

        // --- Adaptive DM pass (preferred) ---
        if (adaptiveMode && state == ScannerState::SCANNING && !justReset) {
            if (scanIndex >= targetCodes)
                break;
            cv::Mat roi = frame(lastAdaptiveROI).clone();

            cv::Mat gray;
            cv::cvtColor(roi, gray, cv::COLOR_BGR2GRAY);

            ZXing::ImageView image(gray.data, gray.cols, gray.rows, ZXing::ImageFormat::Lum);
            auto barcodes = ZXing::ReadBarcodes(image, hints);

            for (const auto& barcode : barcodes) {
                if (!barcode.isValid() || barcode.text().empty())
                    continue;

                std::string normalized = normalizeGS1(barcode.text());

                if (normalized.empty())
                    continue;

                if (seen.count(normalized))
                    continue;

                if (!timingStarted) {
                    timingStarted = true;
                    scanStartTime = std::chrono::steady_clock::now();
                }

                seen.insert(normalized);
                scannedCodes.push_back(normalized);
                ++scanIndex;

                if (normalized.size() < 20) {
                    std::cerr << "⚠ Suspicious code (normalized): " << normalized << std::endl;
                }

                auto pos = barcode.position();
                cv::Rect localRect(
                    static_cast<int>(pos.topLeft().x),
                    static_cast<int>(pos.topLeft().y),
                    static_cast<int>(pos.bottomRight().x - pos.topLeft().x),
                    static_cast<int>(pos.bottomRight().y - pos.topLeft().y)
                );

                localRect.x += lastAdaptiveROI.x;
                localRect.y += lastAdaptiveROI.y;

                lastAdaptiveROI = makeSquareROI(localRect, frame.size());

                cv::rectangle(frame, lastAdaptiveROI, cv::Scalar(0, 180, 255), 2);

                if (scanIndex == targetCodes && !timingStopped) {
                    timingStopped = true;
                    scanEndTime = std::chrono::steady_clock::now();
                    state = ScannerState::READY;
                }

                break;
            }
        }

        // --- Grid scanning ---
        if (gridEnabled) {
            int bw = static_cast<int>(frame.cols * BOX_W);
            int bh = static_cast<int>(frame.rows * BOX_H);
            int bx = (frame.cols - bw) / 2;
            int by = (frame.rows - bh) / 2;
            int cellW = bw / GRID_COLS;
            int cellH = bh / GRID_ROWS;
            
            for (int r = 0; r < GRID_ROWS; ++r) {
                for (int c = 0; c < GRID_COLS; ++c) {
                    if (state != ScannerState::SCANNING || justReset)
                        continue;

                    int idx = r * GRID_COLS + c;
                    if (cellResolved[idx])
                        continue;

                    int cx = bx + c * cellW;
                    int cy = by + r * cellH;
                    cv::Rect cellROI(cx, cy, cellW, cellH);

                    cv::Mat cell = frame(cellROI).clone();
                    cv::Mat cellZoomed;
                    cv::resize(cell, cellZoomed, cv::Size(), CELL_ZOOM, CELL_ZOOM, cv::INTER_LINEAR);

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
                        std::string normalized = normalizeGS1(text);
                        if (normalized.empty())
                            continue;

                        if (scanIndex >= targetCodes)
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
                        if (!adaptiveMode) {
                            adaptiveMode = true;
                            lastAdaptiveROI = makeSquareROI(cellROI, frame.size());
                        }
                        if (state == ScannerState::SCANNING && scanIndex == targetCodes && !timingStopped) {
                            timingStopped = true;
                            scanEndTime = std::chrono::steady_clock::now();
                            state = ScannerState::READY;
                            std::cout << "🟢 State → READY (" << targetCodes << "/" << targetCodes << " scanned)\n";
                            std::cout << "\a\a\a" << std::flush;
                        }
                        if (normalized.size() < 20) {
                            std::cerr << "⚠ Suspicious code (normalized): " << normalized << std::endl;
                        }
                        std::cout << scanIndex << ". " << normalized << std::endl;
                        std::cout << "\a" << std::flush;

                        cellResolved[r * GRID_COLS + c] = true;
                    }
                }
            }
        } else {
            // --- Full frame scanning ---
            if (state == ScannerState::SCANNING && !justReset && scanIndex < targetCodes) {
                cv::Mat gray;
                cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

                ZXing::ImageView image(gray.data, gray.cols, gray.rows, ZXing::ImageFormat::Lum);
                auto barcodes = ZXing::ReadBarcodes(image, hints);

                for (const auto& barcode : barcodes) {
                    if (!barcode.isValid() || barcode.text().empty())
                        continue;

                    const std::string& text = barcode.text();
                    std::string normalized = normalizeGS1(text);
                    if (normalized.empty())
                        continue;

                    if (seen.find(normalized) != seen.end())
                        continue;

                    if (!timingStarted) {
                        timingStarted = true;
                        scanStartTime = std::chrono::steady_clock::now();
                    }

                    seen.insert(normalized);
                    scannedCodes.push_back(normalized);
                    ++scanIndex;

                    if (normalized.size() < 20) {
                        std::cerr << "⚠ Suspicious code (normalized): " << normalized << std::endl;
                    }

                    auto pos = barcode.position();
                    cv::Rect rect(
                        static_cast<int>(pos.topLeft().x),
                        static_cast<int>(pos.topLeft().y),
                        static_cast<int>(pos.bottomRight().x - pos.topLeft().x),
                        static_cast<int>(pos.bottomRight().y - pos.topLeft().y)
                    );
                    cv::rectangle(frame, rect, cv::Scalar(80, 220, 100), 2);

                    std::cout << scanIndex << ". " << normalized << std::endl;
                    std::cout << "\a" << std::flush;

                    if (scanIndex == targetCodes && !timingStopped) {
                        timingStopped = true;
                        scanEndTime = std::chrono::steady_clock::now();
                        state = ScannerState::READY;
                        std::cout << "🟢 СТАТУС → ГОТОВ (" << targetCodes << "/" << targetCodes << " scanned)\n";
                        std::cout << "\a\a\a" << std::flush;
                    }
                }
            }
        }

        // --- State machine ---
        if (state == ScannerState::READY && orderCheckMode) {
            if (fetchOrderInfo(scannedCodes, orderInfo)) {
                orderInfoLoaded = true;
            }
            state = ScannerState::DONE;
        }
        if (state == ScannerState::READY && sendMode == SendMode::AUTO) {
            state = ScannerState::SENDING;
        }

        if (state == ScannerState::SENDING) {
            AggregationResult res = sendAggregation(
                scannedCodes,
                ui_sscc,
                ui_order_id,
                testMode
            );
            lastAggResult = res;

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

            if (res == AggregationResult::OK || res == AggregationResult::ALREADY_EXISTS) {
                boxQueue.push_back({
                    ui_order_id >= 0 ? boxCounter : boxCounter,
                    ui_sscc,
                    ui_order_id,
                    res == AggregationResult::OK ? ScannerState::DONE : ScannerState::READY
                });
                if (boxQueue.size() > 20)
                    boxQueue.erase(boxQueue.begin());
            }

            seen.clear();
            scannedCodes.clear();
            scanIndex = 0;
            timingStarted = timingStopped = false;
        }

        GLuint tex = matToTexture(frame);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Main window - full screen with no decorations
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        
        ImGui::Begin("GRUNDLAGE", nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoBackground
        );

        float sidebar_width = 300.0f;
        float status_height = 50.0f;

        // Top status bar (floating, translucent)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, C_DARK_BG);
        ImGui::BeginChild("StatusBar", ImVec2(ImGui::GetContentRegionAvail().x - sidebar_width - 10.0f, status_height), 
                         false, ImGuiWindowFlags_NoScrollbar);
        
        ImGui::SetCursorPos(ImVec2(20, 15));
        
        // Status indicator with subtle animation
        ImVec4 statusColor = C_DIM;
        const char* statusText = "IDLE";
        const char* statusDesc = "";
        
        switch(state) {
            case ScannerState::SCANNING:
                statusColor = C_ACCENT;
                statusText = "СКАНИРОВАНИЕ";
                statusDesc = "Сканирование кодов…";
                break;
            case ScannerState::READY:
                statusColor = C_WARNING;
                statusText = "ГОТОВ";
                statusDesc = "Готово к отправке";
                break;
            case ScannerState::SENDING:
                statusColor = C_WARNING;
                statusText = "ОТПРАВКА";
                statusDesc = "Отправка...";
                break;
            case ScannerState::DONE:
                statusColor = C_SUCCESS;
                statusText = (lastAggResult == AggregationResult::ALREADY_EXISTS) ? "УЖЕ ОБРАБОТАНО" : "ВЫПОЛНЕНО";
                statusDesc = (lastAggResult == AggregationResult::ALREADY_EXISTS) 
                           ? "Агрегация уже выполнена" 
                           : "Агрегация завершена";
                break;
            case ScannerState::ERROR:
                statusColor = C_ERROR;
                statusText = "ОШИБКА";
                statusDesc = "Ошибка";
                break;
        }
        
        // Status dot with pulsing animation for active states
        if (state == ScannerState::SCANNING || state == ScannerState::SENDING) {
            float time = ImGui::GetTime();
            float alpha = 0.5f + 0.5f * sinf(time * 3.0f);
            statusColor.w = alpha;
        }
        
        ImGui::PushFont(headingFont);
        ImGui::TextColored(statusColor, "●");
        ImGui::PopFont();
        
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2);
        
        ImGui::PushFont(headingFont);
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", statusText);
        ImGui::PopFont();
        
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
        ImGui::PushFont(smallFont);
        ImGui::TextColored(C_DIM, "  %s", statusDesc);
        if (testMode) {
            ImGui::SameLine();
            ImGui::TextColored(C_WARNING, "  • ТЕСТ");
        }

        if (orderCheckMode) {
            ImGui::SameLine();
            ImGui::TextColored(C_ACCENT, "  • ПРОВЕРКА ЗАКАЗА");
        }
        ImGui::PopFont();
        
        // Timer and counter on the right
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
        
        if (timingStarted) {
            auto endTime = timingStopped ? scanEndTime : std::chrono::steady_clock::now();
            float sec = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - scanStartTime).count() / 1000.0f;
            
            ImGui::PushFont(smallFont);
            ImGui::TextColored(C_DIM, "⏱ %.1fs  ", sec);
            ImGui::PopFont();
        }
        
        ImGui::SameLine();
        ImGui::PushFont(smallFont);
        ImGui::TextColored(C_DIM, "📦 %zu/%d", scanIndex, targetCodes);
        ImGui::PopFont();
        
        ImGui::EndChild();
        ImGui::PopStyleColor();
        
        // Main camera view
        ImGui::BeginChild("CameraView", 
                         ImVec2(ImGui::GetContentRegionAvail().x - sidebar_width - 10.0f, 
                                ImGui::GetContentRegionAvail().y - 10.0f), 
                         false, ImGuiWindowFlags_NoScrollbar);
        
        // Center camera feed with aspect ratio preservation
        float availWidth = ImGui::GetContentRegionAvail().x;
        float aspect = (float)frame.rows / (float)frame.cols;
        ImVec2 imageSize(availWidth, availWidth * aspect);
        
        // Center horizontally
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - imageSize.x) * 0.5f);
        // Center vertically with some padding
        ImGui::SetCursorPosY((ImGui::GetContentRegionAvail().y - imageSize.y) * 0.5f);
        
        ImGui::Image((void*)(intptr_t)tex, imageSize);
        
        // Progress bar at bottom (thin, subtle)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);
        float progress = targetCodes > 0 ? scanIndex / (float)targetCodes : 0.0f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, C_ACCENT);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::ProgressBar(progress, ImVec2(-1, 4), "");
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        
        ImGui::EndChild();
        
        ImGui::SameLine();
        
        // Right sidebar - Controls panel
        ImGui::BeginChild("ControlsPanel", ImVec2(sidebar_width, 0), true);
        
        // SECTION: Scanning Configuration
        ImGui::PushFont(headingFont);
        ImGui::Text("Настройки");
        ImGui::PopFont();
        
        ImGui::Separator();
        ImGui::Spacing();
        
        // Target codes slider
        ImGui::PushFont(smallFont);
        ImGui::TextColored(C_DIM, "КОЛ-ВО КОДОВ");
        ImGui::PopFont();
        
        ImGui::PushItemWidth(-1);
        ImGui::SliderInt("##targetCodes", &targetCodes, 1, 10);
        ImGui::PopItemWidth();
        
        ImGui::Spacing();
        
        // Toggles in a compact row
        ImGui::Columns(2, "##toggles", false);
        ImGui::SetColumnWidth(0, 140);
        
        if (ImGui::Checkbox("ГРАНИЦЫ", &gridEnabled)) {
            if (!gridEnabled) {
                adaptiveMode = false;
            }
        }
        
        ImGui::NextColumn();
        ImGui::Checkbox("СЕТКА", &showGridOverlay);
        ImGui::Columns(1);
        
        ImGui::Spacing();
        ImGui::Columns(2, "##modes", false);
        ImGui::SetColumnWidth(0, 140);

        ImGui::Checkbox("ТЕСТ", &testMode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Тестовый режим (без боевой отправки)");

        ImGui::NextColumn();

        ImGui::Checkbox("ПРОВЕРКА", &orderCheckMode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Проверка заказа без агрегации");

        ImGui::Columns(1);

        
        
        // Mode indicators
        if (testMode) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, C_WARNING);
            ImGui::Text("⚠ ТЕСТОВЫЙ РЕЖИМ");
            ImGui::PopStyleColor();
        }

        if (orderCheckMode) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, C_ACCENT);
            ImGui::Text("🔎 ПРОВЕРКА ЗАКАЗА");
            ImGui::PopStyleColor();
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // SECTION: Camera Settings
        ImGui::PushFont(headingFont);
        ImGui::Text("Камера");
        ImGui::PopFont();
        
        ImGui::Separator();
        ImGui::Spacing();
        
        // Camera selector
        ImGui::PushFont(smallFont);
        ImGui::TextColored(C_DIM, "Источник");
        ImGui::PopFont();
        
        static const char* cameraLabels[MAX_CAMERAS];
        static std::string cameraLabelStorage[MAX_CAMERAS];
        for (int i = 0; i < MAX_CAMERAS; ++i) {
            cameraLabelStorage[i] = "Camera " + std::to_string(i);
            cameraLabels[i] = cameraLabelStorage[i].c_str();
        }
        
        ImGui::PushItemWidth(-1);
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
        ImGui::PopItemWidth();
        
        ImGui::Spacing();
        
        // Brightness control
        ImGui::PushFont(smallFont);
        ImGui::TextColored(C_DIM, "Яркость");
        ImGui::PopFont();
        
        ImGui::PushItemWidth(-1);
        ImGui::SliderFloat("##яркость", &claheClipLimit, 1.0f, 6.0f, "%.1f");
        ImGui::PopItemWidth();
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // SECTION: Current Box Stats
        ImGui::PushFont(headingFont);
        ImGui::Text("Текущая коробка");
        ImGui::PopFont();
        
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::Columns(2, "##статистика", false);
        ImGui::SetColumnWidth(0, 120);
        
        ImGui::PushFont(smallFont);
        ImGui::TextColored(C_DIM, "Коды");
        ImGui::PopFont();
        ImGui::Text("%d/%d", lastBoxCodesCount, targetCodes);
        
        ImGui::NextColumn();
        
        ImGui::PushFont(smallFont);
        ImGui::TextColored(C_DIM, "Время");
        ImGui::PopFont();
        ImGui::Text("%.1fs", lastBoxScanTimeSec);
        
        ImGui::Columns(1);
        
        if (lastBoxCodesCount == 0) {
            ImGui::Spacing();
            ImGui::PushFont(smallFont);
            ImGui::TextColored(C_DIM, "ГОТОВ К СКАНИРОВАНИЮ...");
            ImGui::PopFont();
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // SECTION: Order Information (if loaded)
        if (orderInfoLoaded) {
            ImGui::PushFont(headingFont);
            ImGui::Text("Информация о заказе");
            ImGui::PopFont();
            
            ImGui::Separator();
            ImGui::Spacing();
            
            if (orderInfo.mode == "TEST") {
                ImGui::TextColored(C_WARNING, "ТЕСТОВЫЙ ЗАКАЗ");
                ImGui::Spacing();
            }
            
            ImGui::PushFont(smallFont);
            ImGui::TextColored(C_DIM, "Товар");
            ImGui::PopFont();
            ImGui::TextWrapped("%s", orderInfo.product_name.c_str());
            
            ImGui::Spacing();
            
            ImGui::PushFont(smallFont);
            ImGui::TextColored(C_DIM, "Партия");
            ImGui::PopFont();
            ImGui::Text("%s", orderInfo.batch_number.c_str());
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
        
        // SECTION: Aggregation Results
        if (!ui_sscc.empty() || ui_order_id > 0) {
            ImGui::PushFont(headingFont);
            ImGui::Text("Очередь");
            ImGui::PopFont();
            
            ImGui::Separator();
            ImGui::Spacing();
            
            if (!ui_sscc.empty()) {
                ImGui::PushFont(smallFont);
                ImGui::TextColored(C_DIM, "SSCC");
                ImGui::PopFont();
                ImGui::Text("%s", ui_sscc.c_str());
                ImGui::Spacing();
            }
            
            if (ui_order_id > 0) {
                ImGui::PushFont(smallFont);
                ImGui::TextColored(C_DIM, "Order ID");
                ImGui::PopFont();
                ImGui::Text("%d", ui_order_id);
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
        
        // SECTION: Box Queue
        if (!boxQueue.empty()) {
            ImGui::PushFont(headingFont);
            ImGui::Text("ОЧЕРЕДЬ");
            ImGui::PopFont();
            
            ImGui::Separator();
            ImGui::Spacing();
            
            ImGui::BeginChild("QueueList", ImVec2(0, 120), true);
            for (size_t i = 0; i < boxQueue.size(); ++i) {
                const auto& box = boxQueue[i];
                ImVec4 col = box.state == ScannerState::DONE ? C_SUCCESS :
                            box.state == ScannerState::READY ? C_WARNING : C_DIM;
                
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::BulletText("#%zu  %s", i + 1, box.sscc.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
        
        // SECTION: Send Mode
        ImGui::PushFont(headingFont);
        ImGui::Text("В ОБРАБОТКЕ");
        ImGui::PopFont();
        
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::PushItemWidth(-1);
        if (ImGui::BeginCombo("##sendMode", sendMode == SendMode::AUTO ? "АВТОМАТИЧЕСКАЯ ОТПРАВКА" : "РУЧНАЯ ОТПРАВКА")) {
            if (ImGui::Selectable("АВТООТПРАВКА", sendMode == SendMode::AUTO))
                sendMode = SendMode::AUTO;
            if (ImGui::Selectable("РУЧНАЯ ОТПРАВКА", sendMode == SendMode::MANUAL))
                sendMode = SendMode::MANUAL;
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        
        // Manual send button (only when ready)
        if (sendMode == SendMode::MANUAL && state == ScannerState::READY) {
            ImGui::Spacing();
            if (ImGui::Button("ОТПРАВИТЬ", ImVec2(-1, 36))) {
                state = ScannerState::SENDING;
            }
        }
        
        ImGui::Spacing();
        
        // Disable action buttons during sending
        bool disableActions = (state == ScannerState::SENDING);
        if (disableActions) {
            ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
        }

        // SECTION: Action Buttons
        ImGui::PushFont(headingFont);
        ImGui::Text("Действия");
        ImGui::PopFont();
        
        ImGui::Separator();
        ImGui::Spacing();
        
        // New Box button
        if (ImGui::Button("НОВАЯ КОРОБКА", ImVec2(-1, 40))) {
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
            orderInfoLoaded = false;
            orderInfo = {};
        }
        
        ImGui::Spacing();
        
        // Test reset button
        if (ImGui::Button("СБРОС", ImVec2(-1, 32))) {
            if (sendTestReset()) {
                std::cout << "♻ TEST_AGGREGATIONS reset\n";
                seen.clear();
                scannedCodes.clear();
                scanIndex = 0;
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
            }
        }

        if (disableActions) {
            ImGui::PopStyleVar();
            ImGui::PopItemFlag();
        }
        
        ImGui::EndChild();
        
        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.11f, 0.11f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        glDeleteTextures(1, &tex);

        // Keyboard shortcuts
        int key = cv::waitKey(1) & 0xFF;
        if (key == 13 && sendMode == SendMode::MANUAL && state == ScannerState::READY) {
            state = ScannerState::SENDING;
        }
        if (key == 32) { // Space - reset
            seen.clear();
            scannedCodes.clear();
            scanIndex = 0;
            timingStarted = timingStopped = false;
            scanStartTime = scanEndTime = {};
            state = ScannerState::SCANNING;
            adaptiveMode = false;
            std::fill(cellResolved.begin(), cellResolved.end(), false);
            auto now = std::chrono::steady_clock::now();
            std::fill(cellStartTime.begin(), cellStartTime.end(), now);
            orderInfoLoaded = false;
        }
        if (key == 'q' || key == 'Q')
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