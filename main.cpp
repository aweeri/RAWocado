#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <nfd.h>

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <windows.h>
#include <GLFW/glfw3native.h>
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "processing.h"

#ifdef _WIN32
#define IDI_RAWOCADO 101

static void apply_app_icon(GLFWwindow* window) {
    if (!window) return;

    HICON icon = (HICON)LoadImageA(
        GetModuleHandleA(NULL),
        MAKEINTRESOURCEA(IDI_RAWOCADO),
        IMAGE_ICON,
        0,
        0,
        LR_DEFAULTSIZE | LR_SHARED);

    if (!icon) return;

    HWND hwnd = glfwGetWin32Window(window);
    if (!hwnd) return;

    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
}

static GLuint load_ico_texture(const char* ico_path, int desired_size) {
    if (!ico_path || !ico_path[0]) return 0;

    HICON icon = (HICON)LoadImageA(NULL, ico_path, IMAGE_ICON, desired_size, desired_size, LR_LOADFROMFILE);
    if (!icon) return 0;

    int width = desired_size;
    int height = desired_size;

    ICONINFO icon_info = {};
    if (GetIconInfo(icon, &icon_info)) {
        BITMAP bm = {};
        if (icon_info.hbmColor && GetObject(icon_info.hbmColor, sizeof(BITMAP), &bm) > 0) {
            width = bm.bmWidth;
            height = bm.bmHeight;
        }
    }

    HDC screen_dc = GetDC(NULL);
    if (!screen_dc) {
        if (icon_info.hbmColor) DeleteObject(icon_info.hbmColor);
        if (icon_info.hbmMask) DeleteObject(icon_info.hbmMask);
        DestroyIcon(icon);
        return 0;
    }

    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) {
        ReleaseDC(NULL, screen_dc);
        if (icon_info.hbmColor) DeleteObject(icon_info.hbmColor);
        if (icon_info.hbmMask) DeleteObject(icon_info.hbmMask);
        DestroyIcon(icon);
        return 0;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dib_bits = NULL;
    HBITMAP dib = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &dib_bits, NULL, 0);
    if (!dib || !dib_bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mem_dc);
        ReleaseDC(NULL, screen_dc);
        if (icon_info.hbmColor) DeleteObject(icon_info.hbmColor);
        if (icon_info.hbmMask) DeleteObject(icon_info.hbmMask);
        DestroyIcon(icon);
        return 0;
    }

    HGDIOBJ old_obj = SelectObject(mem_dc, dib);
    RECT clear_rect = {0, 0, width, height};
    HBRUSH clear_brush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(mem_dc, &clear_rect, clear_brush);
    DeleteObject(clear_brush);

    DrawIconEx(mem_dc, 0, 0, icon, width, height, 0, NULL, DI_NORMAL);

    std::vector<uint8_t> rgba_pixels((size_t)width * (size_t)height * 4u);
    uint8_t* src = (uint8_t*)dib_bits;
    for (int i = 0; i < width * height; ++i) {
        rgba_pixels[(size_t)i * 4 + 0] = src[(size_t)i * 4 + 2];
        rgba_pixels[(size_t)i * 4 + 1] = src[(size_t)i * 4 + 1];
        rgba_pixels[(size_t)i * 4 + 2] = src[(size_t)i * 4 + 0];
        rgba_pixels[(size_t)i * 4 + 3] = src[(size_t)i * 4 + 3];
    }

    SelectObject(mem_dc, old_obj);
    DeleteObject(dib);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);
    if (icon_info.hbmColor) DeleteObject(icon_info.hbmColor);
    if (icon_info.hbmMask) DeleteObject(icon_info.hbmMask);
    DestroyIcon(icon);

    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba_pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture_id;
}
#endif

static bool params_equal(const EditorParams& a, const EditorParams& b, float eps = 1e-5f) {
    const float* pa = (const float*)&a;
    const float* pb = (const float*)&b;
    int n = (int)(sizeof(EditorParams) / sizeof(float));
    for (int i = 0; i < n; ++i) {
        if (fabsf(pa[i] - pb[i]) > eps) return false;
    }
    return true;
}

static bool save_preset_file(const char* filepath, const EditorParams& p) {
    FILE* f = fopen(filepath, "wb");
    if (!f) return false;
    int written = fprintf(
        f,
        "RAWOCADO_PRESET_V1\n"
        "exposure=%f\ncontrast=%f\nhighlights=%f\nmidtones=%f\nshadows=%f\nwhites=%f\nblacks=%f\n"
        "temperature=%f\ntint=%f\nvibrance=%f\nsaturation=%f\n"
        "texture=%f\nclarity=%f\ndehaze=%f\n"
        "noise_reduction=%f\ncolor_noise_reduction=%f\n",
        p.exposure, p.contrast, p.highlights, p.midtones, p.shadows, p.whites, p.blacks,
        p.temperature, p.tint, p.vibrance, p.saturation,
        p.texture, p.clarity, p.dehaze,
        p.noise_reduction, p.color_noise_reduction);
    fclose(f);
    return written > 0;
}

static bool load_preset_file(const char* filepath, EditorParams* out) {
    if (!out) return false;
    FILE* f = fopen(filepath, "rb");
    if (!f) return false;

    EditorParams p = g_default_params;
    char line[256];
    bool header_ok = false;
    while (fgets(line, sizeof(line), f)) {
        if (!header_ok) {
            if (strstr(line, "RAWOCADO_PRESET_V1") != NULL) {
                header_ok = true;
            }
            continue;
        }

        float v = 0.0f;
        if (sscanf(line, "exposure=%f", &v) == 1) p.exposure = v;
        else if (sscanf(line, "contrast=%f", &v) == 1) p.contrast = v;
        else if (sscanf(line, "highlights=%f", &v) == 1) p.highlights = v;
        else if (sscanf(line, "midtones=%f", &v) == 1) p.midtones = v;
        else if (sscanf(line, "shadows=%f", &v) == 1) p.shadows = v;
        else if (sscanf(line, "whites=%f", &v) == 1) p.whites = v;
        else if (sscanf(line, "blacks=%f", &v) == 1) p.blacks = v;
        else if (sscanf(line, "temperature=%f", &v) == 1) p.temperature = v;
        else if (sscanf(line, "tint=%f", &v) == 1) p.tint = v;
        else if (sscanf(line, "vibrance=%f", &v) == 1) p.vibrance = v;
        else if (sscanf(line, "saturation=%f", &v) == 1) p.saturation = v;
        else if (sscanf(line, "texture=%f", &v) == 1) p.texture = v;
        else if (sscanf(line, "clarity=%f", &v) == 1) p.clarity = v;
        else if (sscanf(line, "dehaze=%f", &v) == 1) p.dehaze = v;
        else if (sscanf(line, "noise_reduction=%f", &v) == 1) p.noise_reduction = v;
        else if (sscanf(line, "color_noise_reduction=%f", &v) == 1) p.color_noise_reduction = v;
    }
    fclose(f);

    if (!header_ok) return false;
    *out = p;
    return true;
}

static bool build_sidecar_path(char* out, size_t out_size, const char* image_path) {
    if (!out || out_size == 0 || !image_path || !image_path[0]) return false;
    int n = snprintf(out, out_size, "%s.rawocado.preset", image_path);
    return n > 0 && (size_t)n < out_size;
}

namespace fs = std::filesystem;

static std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return value;
}

static bool is_supported_raw_path(const std::string& path) {
    fs::path fs_path = fs::u8path(path);
    std::string ext = to_lower_copy(fs_path.extension().string());
    static const char* raw_exts[] = {
        ".arw", ".cr2", ".cr3", ".nef", ".dng", ".raw", ".raf", ".orf",
        ".pef", ".srw", ".x3f", ".rw2", ".3fr", ".iiq", ".kdc", ".mos",
        ".mrw", ".mef", ".erf", ".sr2", ".nrw"
    };
    for (const char* raw_ext : raw_exts) {
        if (ext == raw_ext) return true;
    }
    return false;
}

static void append_raw_files_from_folder(const std::string& folder_path, std::vector<std::string>& out_paths) {
    std::error_code ec;
    fs::path folder = fs::u8path(folder_path);
    if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec)) return;

    for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        std::string candidate = entry.path().string();
        if (is_supported_raw_path(candidate)) {
            out_paths.push_back(candidate);
        }
    }
}

static std::string file_name_for_display(const std::string& path) {
    if (path.empty()) return std::string();
    return fs::u8path(path).filename().string();
}

static std::string truncate_label(const std::string& text, size_t max_chars) {
    if (text.size() <= max_chars) return text;
    if (max_chars <= 3) return std::string("...");
    return text.substr(0, max_chars - 3) + "...";
}

static float srgb_encode_display(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (v <= 0.0031308f) return 12.92f * v;
    return 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

// Draw a combined, Lightroom-like histogram overlaying R/G/B bars and a luma line
static void draw_combined_histogram(const HistogramData& h, ImVec2 pos, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Draw faint background grid
    ImU32 grid_col = IM_COL32(255,255,255,12);
    for (int gx = 0; gx <= 4; gx++) {
        float x = pos.x + (size.x * gx) / 4.0f;
        dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + size.y), grid_col);
    }

    // Draw histogram background and grid
    ImU32 bg_col = IM_COL32(16, 16, 16, 220);
    ImU32 border_col = IM_COL32(255, 255, 255, 25);
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg_col);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border_col);

    float curve_step = size.x / 255.0f;
    float fixed_max = h.max_val * 1.05f;
    if (fixed_max < 1.0f) fixed_max = 1.0f;

    auto get_smoothed = [](const float* arr, int index, int radius) {
        float sum = 0.0f;
        float weight_sum = 0.0f;
        for (int i = -radius; i <= radius; i++) {
            int idx = index + i;
            if (idx >= 0 && idx < 256) {
                float w = (float)(radius + 1 - abs(i));
                sum += arr[idx] * w;
                weight_sum += w;
            }
        }
        return sum / weight_sum;
    };

    auto draw_channel_line = [&](const float* data, ImU32 col, float thickness) {
        ImVec2 pts[256];
        for (int i = 0; i < 256; i++) {
            float smoothed_val = get_smoothed(data, i, 5);
            float x = pos.x + i * curve_step;
            float y = pos.y + size.y * (1.0f - fminf(smoothed_val / fixed_max, 1.0f));
            pts[i] = ImVec2(x, y);
        }
        dl->AddPolyline(pts, 256, col, false, thickness);
    };

    if (g_image.show_r) draw_channel_line(h.r, IM_COL32(220, 60, 60, 220), 1.5f);
    if (g_image.show_g) draw_channel_line(h.g, IM_COL32(60, 220, 60, 220), 1.5f);
    if (g_image.show_b) draw_channel_line(h.b, IM_COL32(60, 120, 220, 220), 1.5f);
    if (g_image.show_luma) draw_channel_line(h.luma, IM_COL32(240, 240, 240, 220), 2.0f);
}

// -----------------------------------------------------------------------------
// Main Loop & UI
// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (!glfwInit()) return -1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "Rawocado", NULL, NULL);
    if (!window) return -1;

#ifdef _WIN32
    apply_app_icon(window);
#endif

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.FontGlobalScale = 1.2f;
    io.FontAllowUserScaling = true;
    ImGui::StyleColorsDark();

    const ImVec4 ui_accent = ImVec4(0.41f, 0.58f, 0.30f, 1.0f);
    const ImVec4 ui_accent_hover = ImVec4(0.47f, 0.63f, 0.35f, 1.0f);
    const ImVec4 ui_accent_active = ImVec4(0.35f, 0.50f, 0.24f, 1.0f);
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.Colors[ImGuiCol_Text] = ImVec4(0.89f, 0.92f, 0.84f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.67f, 0.57f, 1.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.08f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.10f, 0.98f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.22f, 0.27f, 0.18f, 0.60f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.18f, 0.12f, 0.85f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(ui_accent.x, ui_accent.y, ui_accent.z, 0.24f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(ui_accent.x, ui_accent.y, ui_accent.z, 0.34f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.16f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.21f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(ui_accent.x, ui_accent.y, ui_accent.z, 0.78f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(ui_accent_hover.x, ui_accent_hover.y, ui_accent_hover.z, 0.88f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(ui_accent_active.x, ui_accent_active.y, ui_accent_active.z, 0.96f);
    style.Colors[ImGuiCol_Header] = ImVec4(ui_accent.x, ui_accent.y, ui_accent.z, 0.44f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(ui_accent_hover.x, ui_accent_hover.y, ui_accent_hover.z, 0.60f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(ui_accent_active.x, ui_accent_active.y, ui_accent_active.z, 0.72f);
    style.Colors[ImGuiCol_CheckMark] = ui_accent_hover;
    style.Colors[ImGuiCol_SliderGrab] = ui_accent;
    style.Colors[ImGuiCol_SliderGrabActive] = ui_accent_hover;
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(ui_accent_hover.x, ui_accent_hover.y, ui_accent_hover.z, 0.65f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(ui_accent_active.x, ui_accent_active.y, ui_accent_active.z, 0.80f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    std::thread worker(processing_worker);
    std::thread thumb_worker(thumbnail_worker);
    bool processing_pending = false;
    uint64_t pending_request_seq = 0;
    float last_processing_ms = 0.0f;
    bool last_export_success = true;
    auto export_notice_until = std::chrono::steady_clock::time_point::min();
    auto pending_start_time = std::chrono::steady_clock::now();
    std::vector<EditorParams> history;
    history.reserve(256);
    history.push_back(g_params);
    int history_index = 0;
    bool history_dirty = false;
    bool was_any_widget_active = false;
    EditorParams copied_params = g_params;
    bool has_copied_params = false;
    int compare_mode = 1; // 0=Off, 1=Hold(Space), 2=Split
    float compare_split_t = 0.5f;
    bool sidecar_dirty = false;
    bool sidecar_last_ok = true;
    bool was_interacting_last_frame = false;
    float inspector_width = 340.0f;
    float right_panel_width = 280.0f;
    const float panel_gap = 10.0f;
    const float splitter_width = 8.0f;
    const float bottom_bar_height = 190.0f;
    std::vector<std::string> image_queue;
    std::unordered_map<std::string, EditorParams> image_params_by_path;
    std::unordered_map<std::string, EditorParams> image_base_params_by_path;
    std::unordered_map<std::string, GLuint> thumbnail_textures;
    int current_image_index = -1;
    int pending_load_index = -1;
    std::string pending_load_path;
    bool startup_popup_open = true;
    bool startup_popup_opened_once = false;
    GLuint startup_icon_texture = 0;

#ifdef _WIN32
    startup_icon_texture = load_ico_texture("rawocado.ico", 96);
#endif

    auto save_current_image_params = [&]() {
        if (g_load_in_progress.load()) return;
        if (g_current_image_path[0] == '\0') return;
        image_params_by_path[std::string(g_current_image_path)] = g_params;
    };

    auto seed_history_for_current_params = [&]() {
        history.clear();
        history.push_back(g_params);
        history_index = 0;
        history_dirty = false;
        was_any_widget_active = false;
        was_interacting_last_frame = false;
    };

    auto restore_params_for_current_image = [&](const std::string& path) {
        EditorParams restored = g_default_params;
        bool restored_from_sidecar = false;

        auto cached = image_params_by_path.find(path);
        if (cached != image_params_by_path.end()) {
            restored = cached->second;
        } else {
            char sidecar_path[1300] = {0};
            if (build_sidecar_path(sidecar_path, sizeof(sidecar_path), path.c_str())) {
                restored = g_params;
                if (load_preset_file(sidecar_path, &restored)) {
                    restored_from_sidecar = true;
                } else {
                    restored = g_default_params;
                }
            }
        }

        if (!restored_from_sidecar && cached == image_params_by_path.end()) {
            restored.temperature = g_image.image_temperature;
        }

        if (image_base_params_by_path.find(path) == image_base_params_by_path.end()) {
            image_base_params_by_path[path] = restored;
        }

        g_params = restored;
        sidecar_dirty = false;
        sidecar_last_ok = true;
        seed_history_for_current_params();
    };

    auto begin_image_load = [&](int index) {
        if (index < 0 || index >= (int)image_queue.size()) return;
        if (g_load_in_progress.load()) return;

        save_current_image_params();

        pending_load_index = index;
        pending_load_path = image_queue[(size_t)index];
        g_load_progress = 0.0f;
        g_load_done = false;
        g_load_success = false;
        request_thumbnail_preview(pending_load_path.c_str());
        request_load_raw_async(pending_load_path.c_str());
    };

    auto add_multiple_paths = [&](const std::vector<std::string>& paths, bool select_first_new) {
        bool selected = false;
        for (const std::string& path : paths) {
            if (path.empty() || !is_supported_raw_path(path)) continue;
            auto existing = std::find(image_queue.begin(), image_queue.end(), path);
            if (existing == image_queue.end()) {
                image_queue.push_back(path);
            }
            request_thumbnail_preview(path.c_str());
            if (select_first_new && !selected) {
                begin_image_load((int)(std::find(image_queue.begin(), image_queue.end(), path) - image_queue.begin()));
                selected = true;
            }
        }
    };

    // Load a test image if provided via CLI
    if (argc > 1) {
        image_queue.push_back(argv[1]);
        request_thumbnail_preview(argv[1]);
        begin_image_load(0);
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (!startup_popup_opened_once) {
            ImGui::OpenPopup("Welcome to Rawocado");
            startup_popup_opened_once = true;
        }

        ImGui::SetNextWindowSize(ImVec2(620.0f, 0.0f), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (startup_popup_open && ImGui::BeginPopupModal("Welcome to Rawocado", &startup_popup_open, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (startup_icon_texture != 0) {
                ImGui::Image((void*)(intptr_t)startup_icon_texture, ImVec2(84.0f, 84.0f));
            } else {
                ImGui::Dummy(ImVec2(84.0f, 84.0f));
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextColored(ui_accent_hover, "RAWOCADO");
            ImGui::Text("Made by Aweeri");
            ImGui::TextWrapped("A fast, experimental RAW photo editor focused on responsive controls and real-time visual feedback.");
            ImGui::EndGroup();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.28f, 1.0f), "PROTOTYPE WARNING");
            ImGui::TextWrapped("This software is a very early prototype. Nothing is perfect, behavior may change, and things will break between versions.");
            ImGui::TextWrapped("Use with caution and remember that not all features are fully implemented yet ^^ ");

            ImGui::Spacing();
            if (ImGui::Button("I Understand", ImVec2(180.0f, 30.0f))) {
                startup_popup_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (g_load_done.exchange(false)) {
            if (g_load_success.load()) {
                upload_loaded_image_textures();
                current_image_index = pending_load_index;
                if (!pending_load_path.empty()) {
                    restore_params_for_current_image(pending_load_path);
                    image_params_by_path[pending_load_path] = g_params;
                }
                g_image.zoom = 1.0f;
                g_image.last_zoom = 1.0f;
                g_image.pan_x = 0.0f;
                g_image.pan_y = 0.0f;
                g_full_refresh_requested = true;
                request_processing();
                processing_pending = true;
                pending_request_seq = g_request_seq.load(std::memory_order_relaxed);
                pending_start_time = std::chrono::steady_clock::now();
                pending_load_index = -1;
                pending_load_path.clear();
            } else {
                sidecar_last_ok = false;
                pending_load_index = -1;
                pending_load_path.clear();
            }
        }

        ThumbnailPreview ready_preview = {};
        while (pop_thumbnail_preview(&ready_preview)) {
            GLuint texture_id = 0;
            glGenTextures(1, &texture_id);
            glBindTexture(GL_TEXTURE_2D, texture_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ready_preview.width, ready_preview.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, ready_preview.rgba_pixels.data());
            glBindTexture(GL_TEXTURE_2D, 0);
            thumbnail_textures[std::string(ready_preview.path)] = texture_id;
        }

    bool params_changed = false;
    bool viewport_changed = false;
    bool should_request_processing = false;
    bool is_interacting_now = false;

        auto hint_for_last_item = [](const char* hint) {
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", hint);
            }
        };

        const float min_inspector_width = 340.0f;
        const float min_right_width = 180.0f;
        const float min_viewport_width = 220.0f;
        float usable_height = io.DisplaySize.y - bottom_bar_height;
        if (usable_height < 240.0f) usable_height = io.DisplaySize.y;

        float max_inspector_width = io.DisplaySize.x - right_panel_width - panel_gap * 2.0f - min_viewport_width;
        if (max_inspector_width < min_inspector_width) max_inspector_width = min_inspector_width;
        if (inspector_width < min_inspector_width) inspector_width = min_inspector_width;
        if (inspector_width > max_inspector_width) inspector_width = max_inspector_width;

        float max_right_width = io.DisplaySize.x - inspector_width - panel_gap * 2.0f - min_viewport_width;
        if (max_right_width < min_right_width) max_right_width = min_right_width;
        if (right_panel_width < min_right_width) right_panel_width = min_right_width;
        if (right_panel_width > max_right_width) right_panel_width = max_right_width;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, usable_height));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##LayoutSplitters", NULL,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        float left_splitter_x = inspector_width + panel_gap * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(left_splitter_x - splitter_width * 0.5f, 0.0f));
        ImGui::InvisibleButton("##LeftSplitter", ImVec2(splitter_width, io.DisplaySize.y));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive()) {
            inspector_width += io.MouseDelta.x;
            if (inspector_width < min_inspector_width) inspector_width = min_inspector_width;
            if (inspector_width > max_inspector_width) inspector_width = max_inspector_width;
        }

        float right_splitter_x = io.DisplaySize.x - right_panel_width - panel_gap * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(right_splitter_x - splitter_width * 0.5f, 0.0f));
        ImGui::InvisibleButton("##RightSplitter", ImVec2(splitter_width, io.DisplaySize.y));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (ImGui::IsItemActive()) {
            right_panel_width -= io.MouseDelta.x;
            if (right_panel_width < min_right_width) right_panel_width = min_right_width;
            if (right_panel_width > max_right_width) right_panel_width = max_right_width;
        }
        ImGui::End();

        // UI Panel
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(inspector_width, 0));
        ImGui::SetNextWindowSizeConstraints(ImVec2(min_inspector_width, 0), ImVec2(max_inspector_width, usable_height));
        ImGui::Begin("Inspector", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        // Reserve exactly 170 pixels on the right side for labels so they don't get cut off
        ImGui::PushItemWidth(-170.0f);

        // Keep manual slider input safely in-range.
        ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;

        if (g_image.proxy_buffer != NULL) {
            if (ImGui::CollapsingHeader("Histogram", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                ImVec2 size = ImVec2(avail.x, 120.0f);
                ImGui::Dummy(size);
                ImVec2 pos = ImGui::GetItemRectMin();
                draw_combined_histogram(g_image.histogram, pos, size);
                
                ImGui::Spacing();
                ImGui::Checkbox("R", &g_image.show_r); ImGui::SameLine();
                ImGui::Checkbox("G", &g_image.show_g); ImGui::SameLine();
                ImGui::Checkbox("B", &g_image.show_b); ImGui::SameLine();
                ImGui::Checkbox("Luma", &g_image.show_luma);
            }

        }

        if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            params_changed |= ImGui::SliderFloat("Exposure", &g_params.exposure, -5.0f, 5.0f, "%.2f EV", flags);
            hint_for_last_item("Global brightness in EV. 0 keeps camera baseline.");
            params_changed |= ImGui::SliderFloat("Contrast", &g_params.contrast, -100.0f, 100.0f, "%.0f", flags);
            params_changed |= ImGui::SliderFloat("Highlights", &g_params.highlights, -100.0f, 100.0f, "%.0f", flags);
            params_changed |= ImGui::SliderFloat("Midtones", &g_params.midtones, -100.0f, 100.0f, "%.0f", flags);
            params_changed |= ImGui::SliderFloat("Shadows", &g_params.shadows, -100.0f, 100.0f, "%.0f", flags);
            params_changed |= ImGui::SliderFloat("Whites", &g_params.whites, -100.0f, 100.0f, "%.0f", flags);
            params_changed |= ImGui::SliderFloat("Blacks", &g_params.blacks, -100.0f, 100.0f, "%.0f", flags);
        }

        if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Log scale gives better feel in the common WB range.
            params_changed |= ImGui::SliderFloat("Temperature", &g_params.temperature, 2000.0f, 15000.0f, "%.0f K", flags | ImGuiSliderFlags_Logarithmic);
            hint_for_last_item("White balance temperature. Lower is cooler, higher is warmer.");
            params_changed |= ImGui::SliderFloat("Tint", &g_params.tint, -150.0f, 150.0f, "%.0f", flags);
            params_changed |= ImGui::SliderFloat("Vibrance", &g_params.vibrance, -100.0f, 100.0f, "%.0f", flags);
            params_changed |= ImGui::SliderFloat("Saturation", &g_params.saturation, -100.0f, 100.0f, "%.0f", flags);
        }

        if (ImGui::CollapsingHeader("Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
            params_changed |= ImGui::SliderFloat("Texture", &g_params.texture, -100.0f, 100.0f, "%.0f", flags);
            params_changed |= ImGui::SliderFloat("Clarity", &g_params.clarity, -100.0f, 100.0f, "%.0f", flags);
            hint_for_last_item("Midtone contrast boost for detail and punch.");
            params_changed |= ImGui::SliderFloat("Dehaze", &g_params.dehaze, -100.0f, 100.0f, "%.0f", flags);
        }

        if (ImGui::CollapsingHeader("Detail", ImGuiTreeNodeFlags_DefaultOpen)) {
            params_changed |= ImGui::SliderFloat("Noise Reduction", &g_params.noise_reduction, 0.0f, 100.0f, "%.0f", flags);
            hint_for_last_item("Higher values smooth grain but can reduce fine detail.");
            params_changed |= ImGui::SliderFloat("Color Noise Reduction", &g_params.color_noise_reduction, 0.0f, 100.0f, "%.0f", flags);
        }

        if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
            int decimation_value = g_image.lod_strength;
            if (decimation_value < 0) decimation_value = 0;
            if (decimation_value > 8) decimation_value = 8;
            if (ImGui::SliderInt("Decimation", &decimation_value, 0, 8, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                if ((decimation_value & 1) != 0) {
                    decimation_value = (decimation_value < 8) ? (decimation_value + 1) : 8;
                }
                g_image.lod_strength = decimation_value;
                params_changed = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = full resolution. Larger values divide the source more aggressively at fit/zoomed-out view.");
            
            params_changed |= ImGui::SliderInt("Histogram Quality", &g_image.histogram_quality, 1, 16, "1/%d");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Higher = faster histogram, lower precision.");

            ImGui::SliderInt("Histogram Interval (ms)", &g_image.histogram_interval_ms, 30, 500, "%d ms");
            hint_for_last_item("Lower is more responsive but costs more CPU.");

            int proc_step = g_current_processing_step.load();
            float effective_decimation = g_current_effective_decimation.load();
            int adaptive_pixels = g_current_adaptive_pixels.load();
            ImGui::Separator();
            ImGui::Text("Current Step: %dx", proc_step);
            ImGui::Text("Effective Decimation: %.2fx", effective_decimation);
            ImGui::Text("Adaptive Pixels: %.2f MP", (float)adaptive_pixels / 1000000.0f);
        }

        if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* compare_modes[] = {"Off", "Hold (Space)", "Split"};
            ImGui::Combo("Compare Mode", &compare_mode, compare_modes, IM_ARRAYSIZE(compare_modes));
            if (compare_mode == 2) {
                ImGui::SliderFloat("Split Position", &compare_split_t, 0.10f, 0.90f, "%.2f");
                hint_for_last_item("Drag where before/after comparison split appears.");
            }

            float view_fit_zoom = 1.0f;
            float view_1x_zoom = 1.0f;
            float view_2x_zoom = 2.0f;
            float view_width = ImGui::GetContentRegionAvail().x;
            if (view_width > 1.0f && g_image.width > 0) {
                view_1x_zoom = (float)g_image.width / view_width;
                view_2x_zoom = view_1x_zoom * 2.0f;
            }

            if (ImGui::Button("Fit", ImVec2(-1, 22))) {
                g_image.zoom = view_fit_zoom;
                viewport_changed = true;
            }
            if (ImGui::Button("1:1", ImVec2(-1, 22))) {
                g_image.zoom = view_1x_zoom;
                viewport_changed = true;
            }
            if (ImGui::Button("2:1", ImVec2(-1, 22))) {
                g_image.zoom = view_2x_zoom;
                viewport_changed = true;
            }
        }

        if (ImGui::CollapsingHeader("Tools & Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
            float avail_w = ImGui::GetContentRegionAvail().x;
            float half_w = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

            if (ImGui::Button("Reset Parameters", ImVec2(-1, 26))) {
                g_params = g_default_params;
                if (g_image.proxy_buffer) {
                    g_params.temperature = g_image.image_temperature;
                }
                params_changed = true;
                g_image.zoom = 1.0f;
                g_full_refresh_requested = true;
                debug_log("ui reset parameters -> full refresh");
            }
            
            bool can_undo = history_index > 0;
            bool can_redo = history_index + 1 < (int)history.size();
            
            if (!can_undo) ImGui::BeginDisabled();
            if (ImGui::Button("Undo", ImVec2(half_w, 24))) {
                history_index--;
                g_params = history[(size_t)history_index];
                params_changed = true;
                history_dirty = false;
                g_full_refresh_requested = true;
                debug_log("ui undo -> history_index=%d full refresh", history_index);
            }
            if (!can_undo) ImGui::EndDisabled();
            
            ImGui::SameLine();
            
            if (!can_redo) ImGui::BeginDisabled();
            if (ImGui::Button("Redo", ImVec2(half_w, 24))) {
                history_index++;
                g_params = history[(size_t)history_index];
                params_changed = true;
                history_dirty = false;
                g_full_refresh_requested = true;
                debug_log("ui redo -> history_index=%d full refresh", history_index);
            }
            if (!can_redo) ImGui::EndDisabled();

            if (ImGui::Button("Copy Settings", ImVec2(half_w, 24))) {
                copied_params = g_params;
                has_copied_params = true;
            }
            ImGui::SameLine();
            if (!has_copied_params) ImGui::BeginDisabled();
            if (ImGui::Button("Paste Settings", ImVec2(half_w, 24))) {
                g_params = copied_params;
                params_changed = true;
                debug_log("ui paste settings");
            }
            if (!has_copied_params) ImGui::EndDisabled();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGui::Button("Load Preset", ImVec2(half_w, 24))) {
                nfdchar_t* outPath = NULL;
                nfdresult_t result = NFD_OpenDialog("preset,txt", NULL, &outPath);
                if (result == NFD_OKAY) {
                    EditorParams loaded = g_params;
                    if (load_preset_file(outPath, &loaded)) {
                        g_params = loaded;
                        params_changed = true;
                        sidecar_dirty = true;
                    }
                    free(outPath);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Preset", ImVec2(half_w, 24))) {
                nfdchar_t* outPath = NULL;
                nfdresult_t result = NFD_SaveDialog("preset,txt", NULL, &outPath);
                if (result == NFD_OKAY) {
                    sidecar_last_ok = save_preset_file(outPath, g_params);
                    free(outPath);
                }
            }

            char sidecar_path[1300] = {0};
            bool has_sidecar_target = build_sidecar_path(sidecar_path, sizeof(sidecar_path), g_current_image_path);
            
            if (!has_sidecar_target) ImGui::BeginDisabled();
            if (ImGui::Button("Load Sidecar", ImVec2(half_w, 24))) {
                EditorParams loaded = g_params;
                if (load_preset_file(sidecar_path, &loaded)) {
                    g_params = loaded;
                    params_changed = true;
                    sidecar_last_ok = true;
                    sidecar_dirty = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Sidecar", ImVec2(half_w, 24))) {
                sidecar_last_ok = save_preset_file(sidecar_path, g_params);
                if (sidecar_last_ok) sidecar_dirty = false;
            }
            if (!has_sidecar_target) ImGui::EndDisabled();
            
            if (sidecar_dirty || !sidecar_last_ok) {
                ImGui::TextDisabled("Sidecar: %s", !sidecar_last_ok ? "Write Failed" : "Unsaved Changes");
            }
        }

        if (ImGui::CollapsingHeader("File & Export", ImGuiTreeNodeFlags_DefaultOpen)) {
            float avail_w = ImGui::GetContentRegionAvail().x;
            float half_w = (avail_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

            const bool load_controls_disabled = g_load_in_progress.load();
            if (load_controls_disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Add Images", ImVec2(half_w, 30))) {
                nfdpathset_t pathset;
                nfdresult_t result = NFD_OpenDialogMultiple("arw,cr2,cr3,nef,dng,raw,raf,orf,pef,srw,x3f,rw2,3fr,iiq,kdc,mos,mrw,mef,erf,sr2,nrw", NULL, &pathset);
                if (result == NFD_OKAY) {
                    std::vector<std::string> paths;
                    size_t count = NFD_PathSet_GetCount(&pathset);
                    paths.reserve(count);
                    for (size_t i = 0; i < count; ++i) {
                        nfdchar_t* selected = NFD_PathSet_GetPath(&pathset, i);
                        if (selected && selected[0]) paths.push_back(selected);
                    }
                    NFD_PathSet_Free(&pathset);
                    add_multiple_paths(paths, image_queue.empty() || current_image_index < 0);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Folder", ImVec2(half_w, 30))) {
                nfdchar_t* outPath = NULL;
                nfdresult_t result = NFD_PickFolder(NULL, &outPath);
                if (result == NFD_OKAY) {
                    std::vector<std::string> folder_paths;
                    append_raw_files_from_folder(outPath, folder_paths);
                    free(outPath);
                    add_multiple_paths(folder_paths, image_queue.empty() || current_image_index < 0);
                }
            }
            if (load_controls_disabled) ImGui::EndDisabled();

            if (image_queue.empty()) {
                ImGui::TextDisabled("Add some RAW files/folder to start.");
            } else {
                ImGui::TextDisabled("Loaded images: %d", (int)image_queue.size());
            }

            ImGui::Spacing();

            const bool export_controls_disabled = (g_image.proxy_buffer == NULL) || g_export_in_progress.load() || g_load_in_progress.load();
            if (export_controls_disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Export Image", ImVec2(-1, 30))) {
                if (g_image.proxy_buffer) {
                    nfdchar_t *outPath = NULL;
                    nfdresult_t result = NFD_SaveDialog("png,jpg,jpeg", NULL, &outPath);
                    if (result == NFD_OKAY) {
                        request_export_async(outPath);
                        free(outPath);
                    }
                }
            }
            if (export_controls_disabled) ImGui::EndDisabled();

            if (g_export_in_progress.load()) {
                ImGui::Text("Export: running...");
                ImGui::ProgressBar(g_export_progress.load(), ImVec2(-1, 0));
            } else if (g_export_done.exchange(false)) {
                last_export_success = g_export_success.load();
                export_notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            }

            if (!g_export_in_progress.load() && std::chrono::steady_clock::now() < export_notice_until) {
                ImGui::Text("Export: %s", last_export_success ? "done" : "failed");
            }
        }

        if (ImGui::CollapsingHeader("System Status")) {
            const char* status_text = g_load_in_progress.load() ? "Loading" : (processing_pending ? "Processing" : "Idle");
            ImGui::Text("Status: %s", status_text);
            ImGui::Text("Source: %s", g_image.using_proxy_source ? "Proxy" : "Full");
            ImGui::Text("History steps: %d / %d", history_index + 1, (int)history.size());
            ImGui::TextDisabled("Render: %.1f ms", last_processing_ms);
            ImGui::TextDisabled("Worker: %.1f ms", g_last_process_ms.load());
            ImGui::TextDisabled("Hist pass: %.1f ms", g_last_histogram_ms.load());
        }

        ImGui::PopItemWidth();
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - bottom_bar_height));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, bottom_bar_height));
        ImGui::Begin("Image Strip", NULL,
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoScrollbar);

        if (g_load_in_progress.load()) {
            ImGui::Text("Loading: %s", file_name_for_display(pending_load_path).c_str());
            ImGui::ProgressBar(g_load_progress.load(), ImVec2(-1, 0));
        }
        if (g_export_in_progress.load()) {
            if (g_load_in_progress.load()) ImGui::Spacing();
            ImGui::Text("Exporting image");
            ImGui::ProgressBar(g_export_progress.load(), ImVec2(-1, 0));
        }

        if (!g_load_in_progress.load() && !g_export_in_progress.load()) {
            ImGui::TextDisabled("Add images or a folder, then click a file to switch.");
        }

        ImGui::Separator();
        ImGui::BeginChild("##ImageStripScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        const ImVec2 thumb_size(96.0f, 72.0f);
        int strip_remove_index = -1;
        bool strip_reset_target = false;
        std::string strip_target_path;

        ImGuiStyle& strip_style = ImGui::GetStyle();
        float old_spacing_x = strip_style.ItemSpacing.x;
        strip_style.ItemSpacing.x = 6.0f;
        for (size_t i = 0; i < image_queue.size(); ++i) {
            const std::string& path = image_queue[i];
            std::string label = file_name_for_display(path);
            if (label.empty()) label = path;
            std::string short_label = truncate_label(label, 16);
            bool selected = ((int)i == current_image_index);

            EditorParams current_for_tile = g_default_params;
            bool has_current_for_tile = false;
            if (selected) {
                current_for_tile = g_params;
                has_current_for_tile = true;
            } else {
                auto params_it = image_params_by_path.find(path);
                if (params_it != image_params_by_path.end()) {
                    current_for_tile = params_it->second;
                    has_current_for_tile = true;
                }
            }
            bool edited_marker = false;
            auto base_it = image_base_params_by_path.find(path);
            if (has_current_for_tile && base_it != image_base_params_by_path.end()) {
                edited_marker = !params_equal(current_for_tile, base_it->second);
            }

            ImGui::BeginGroup();
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ui_accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_accent_hover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui_accent_active);
            }

            bool clicked = false;
            auto thumb_it = thumbnail_textures.find(path);
            if (thumb_it != thumbnail_textures.end()) {
                ImTextureRef thumb_ref((ImTextureID)(intptr_t)thumb_it->second);
                clicked = ImGui::ImageButton(("##thumb" + std::to_string(i)).c_str(), thumb_ref, thumb_size);
            } else {
                clicked = ImGui::Button(("##thumb" + std::to_string(i)).c_str(), thumb_size);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 min = ImGui::GetItemRectMin();
                ImVec2 max = ImGui::GetItemRectMax();
                dl->AddRectFilled(min, max, IM_COL32(42, 42, 42, 255), 6.0f);
                dl->AddRect(min, max, IM_COL32(255, 255, 255, 24), 6.0f);
                dl->AddText(ImVec2(min.x + 10.0f, min.y + 26.0f), IM_COL32(220, 220, 220, 255), "Loading");
            }

            if (edited_marker) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 min = ImGui::GetItemRectMin();
                ImVec2 marker_center = ImVec2(min.x + 10.0f, min.y + 10.0f);
                dl->AddCircleFilled(marker_center, 4.0f, IM_COL32(245, 190, 82, 255), 16);
            }

            if (ImGui::BeginPopupContextItem(("##thumb_ctx" + std::to_string(i)).c_str())) {
                if (ImGui::MenuItem("Reset Changes For This Image")) {
                    strip_target_path = path;
                    strip_reset_target = true;
                }
                if (ImGui::MenuItem("Remove From Imported List")) {
                    strip_remove_index = (int)i;
                }
                ImGui::EndPopup();
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", path.c_str());
            }

            if (clicked && !g_load_in_progress.load() && (int)i != current_image_index) {
                begin_image_load((int)i);
            }

            ImGui::TextUnformatted(short_label.c_str());
            if (selected) {
                ImGui::PopStyleColor(3);
            }
            ImGui::EndGroup();
            if (i + 1 < image_queue.size()) {
                ImGui::SameLine();
            }
        }
        strip_style.ItemSpacing.x = old_spacing_x;
        ImGui::EndChild();

        if (strip_reset_target && !strip_target_path.empty()) {
            auto base_it = image_base_params_by_path.find(strip_target_path);
            if (base_it != image_base_params_by_path.end()) {
                image_params_by_path[strip_target_path] = base_it->second;
                if (current_image_index >= 0 && current_image_index < (int)image_queue.size() && image_queue[(size_t)current_image_index] == strip_target_path) {
                    g_params = base_it->second;
                    seed_history_for_current_params();
                    g_full_refresh_requested = true;
                    request_processing();
                    processing_pending = true;
                    pending_request_seq = g_request_seq.load(std::memory_order_relaxed);
                    pending_start_time = std::chrono::steady_clock::now();
                }
            }
        }

        if (strip_remove_index >= 0 && strip_remove_index < (int)image_queue.size()) {
            std::string removed_path = image_queue[(size_t)strip_remove_index];
            auto thumb_it = thumbnail_textures.find(removed_path);
            if (thumb_it != thumbnail_textures.end()) {
                glDeleteTextures(1, &thumb_it->second);
                thumbnail_textures.erase(thumb_it);
            }
            image_params_by_path.erase(removed_path);
            image_base_params_by_path.erase(removed_path);
            image_queue.erase(image_queue.begin() + strip_remove_index);

            if (image_queue.empty()) {
                current_image_index = -1;
            } else if (current_image_index == strip_remove_index) {
                int next_index = strip_remove_index;
                if (next_index >= (int)image_queue.size()) next_index = (int)image_queue.size() - 1;
                begin_image_load(next_index);
            } else if (current_image_index > strip_remove_index) {
                current_image_index--;
            }
        }

        ImGui::End();

        // Image Viewport
        if (g_image.texture_id != 0) {
            bool preview_original = false;
            if (compare_mode == 1) {
                preview_original = ImGui::IsKeyDown(ImGuiKey_Space) && (g_image.original_texture_id != 0);
            }
            GLuint display_texture = preview_original ? g_image.original_texture_id : g_image.texture_id;

            float panel_width = io.DisplaySize.x - inspector_width - panel_gap * 2.0f - right_panel_width;
            float panel_height = usable_height;
            float right_panel_height = usable_height;

            ImGui::SetNextWindowPos(ImVec2(inspector_width + panel_gap, 0));
            ImGui::SetNextWindowSize(ImVec2(panel_width, panel_height));
            ImGui::Begin("Viewport", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

            ImGui::Text("Viewport");
            ImGui::Separator();
            // Zoom with scroll and pan by dragging the image

            float aspect = (float)g_image.width / (float)g_image.height;
            float available_width = ImGui::GetContentRegionAvail().x;
            float image_w = available_width * g_image.zoom;
            float image_h = image_w / aspect;
            ImGui::SetNextWindowContentSize(ImVec2(image_w, image_h));
            ImGui::BeginChild("ImageScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollWithMouse);
            ImGuiIO& io = ImGui::GetIO();
            
            ImVec2 child_window_pos = ImGui::GetWindowPos();
            ImVec2 child_window_size = ImGui::GetWindowSize();
            
            // Scroll wheel zoom centered on mouse cursor
            if (ImGui::IsWindowHovered()) {
                if (io.MouseWheel != 0.0f) {
                    is_interacting_now = true;
                    float old_zoom = g_image.zoom;
                    float zoom_step = 1.14f;
                    float new_zoom = g_image.zoom * powf(zoom_step, io.MouseWheel);
                    if (new_zoom < 0.1f) new_zoom = 0.1f;
                    if (new_zoom > 64.0f) new_zoom = 64.0f;
                    if (new_zoom != old_zoom) {
                        ImVec2 mouse_rel = ImVec2(io.MousePos.x - child_window_pos.x, io.MousePos.y - child_window_pos.y);
                        if (mouse_rel.x >= 0.0f && mouse_rel.y >= 0.0f && mouse_rel.x <= child_window_size.x && mouse_rel.y <= child_window_size.y) {
                            ImVec2 image_point = ImVec2((ImGui::GetScrollX() + mouse_rel.x) / old_zoom, (ImGui::GetScrollY() + mouse_rel.y) / old_zoom);
                            g_image.zoom = new_zoom;
                            ImGui::SetScrollX(image_point.x * g_image.zoom - mouse_rel.x);
                            ImGui::SetScrollY(image_point.y * g_image.zoom - mouse_rel.y);
                        } else {
                            g_image.zoom = new_zoom;
                        }
                        viewport_changed = true;
                    }
                }
            }
            
            // Click+drag to pan
            if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                is_interacting_now = true;
                ImVec2 md = io.MouseDelta;
                ImGui::SetScrollX(ImGui::GetScrollX() - md.x);
                ImGui::SetScrollY(ImGui::GetScrollY() - md.y);
            }
            
            ImGui::Image((void*)(intptr_t)display_texture, ImVec2(image_w, image_h));
            if (compare_mode == 2 && g_image.original_texture_id != 0) {
                ImVec2 img_min = ImGui::GetItemRectMin();
                ImVec2 img_max = ImGui::GetItemRectMax();
                float split_x = img_min.x + (img_max.x - img_min.x) * compare_split_t;

                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(img_min, ImVec2(split_x, img_max.y), true);
                dl->AddImage((void*)(intptr_t)g_image.original_texture_id, img_min, img_max, ImVec2(0, 0), ImVec2(1, 1));
                dl->PopClipRect();

                dl->AddLine(ImVec2(split_x, img_min.y), ImVec2(split_x, img_max.y), IM_COL32(255, 255, 255, 180), 2.0f);
            }
            
            // Viewport culling calculation
            float scroll_x = ImGui::GetScrollX();
            float scroll_y = ImGui::GetScrollY();
            float view_w = ImGui::GetWindowWidth();
            float view_h = ImGui::GetWindowHeight();

            float scale_x = (float)g_image.width / image_w;
            float scale_y = (float)g_image.height / image_h;

            int current_vis_x_start = (int)(scroll_x * scale_x);
            int current_vis_y_start = (int)(scroll_y * scale_y);
            int current_vis_x_end = (int)((scroll_x + view_w) * scale_x);
            int current_vis_y_end = (int)((scroll_y + view_h) * scale_y);
            if (current_vis_x_end > g_image.width) current_vis_x_end = g_image.width;
            if (current_vis_y_end > g_image.height) current_vis_y_end = g_image.height;

            // Re-render if scrolling exposes a region outside our padded cached bounds, or if zoom changes
            if (current_vis_x_start < g_image.vis_x_start || current_vis_y_start < g_image.vis_y_start ||
                current_vis_x_end > g_image.vis_x_end || current_vis_y_end > g_image.vis_y_end ||
                g_image.zoom != g_image.last_zoom) {
                
                // Increase padding for smoother panning, but not so large it causes a massive lag spike
                int pad_x = (int)(view_w * 0.25f * scale_x);
                int pad_y = (int)(view_h * 0.25f * scale_y);
                
                g_image.vis_x_start = current_vis_x_start - pad_x;
                g_image.vis_y_start = current_vis_y_start - pad_y;
                g_image.vis_x_end = current_vis_x_end + pad_x;
                g_image.vis_y_end = current_vis_y_end + pad_y;
                
                if (g_image.vis_x_start < 0) g_image.vis_x_start = 0;
                if (g_image.vis_y_start < 0) g_image.vis_y_start = 0;
                if (g_image.vis_x_end > g_image.width) g_image.vis_x_end = g_image.width;
                if (g_image.vis_y_end > g_image.height) g_image.vis_y_end = g_image.height;
                
                g_image.last_zoom = g_image.zoom;
                viewport_changed = true;
            }
            
            ImGui::EndChild();
            ImGui::End();
            ImGui::SetNextWindowPos(ImVec2(inspector_width + panel_gap + panel_width + panel_gap, 0));
            ImGui::SetNextWindowSize(ImVec2(right_panel_width, right_panel_height));
            ImGui::Begin("Info", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

            if (ImGui::CollapsingHeader("Histogram", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                ImVec2 size = ImVec2(avail.x, 120.0f);
                ImGui::Dummy(size);
                ImVec2 pos = ImGui::GetItemRectMin();
                draw_combined_histogram(g_image.histogram, pos, size);
            }

            if (ImGui::CollapsingHeader("Metadata", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Working Source: %dx%d", g_image.width, g_image.height);
                ImGui::Text("Full Resolution: %dx%d", g_image.full_width, g_image.full_height);
                ImGui::Separator();
                ImGui::Text("Camera: %s %s", g_image.metadata.camera_maker, g_image.metadata.camera_model);
                if (g_image.metadata.lens_name[0] != '\0') {
                    ImGui::Text("Lens: %s", g_image.metadata.lens_name);
                }
                ImGui::Separator();
                ImGui::Text("ISO: %.0f", g_image.metadata.iso_speed);
                if (g_image.metadata.shutter > 0.0f) {
                    if (g_image.metadata.shutter < 1.0f) {
                        ImGui::Text("Shutter: 1/%.0f s", 1.0f / g_image.metadata.shutter);
                    } else {
                        ImGui::Text("Shutter: %.1f s", g_image.metadata.shutter);
                    }
                }
                ImGui::Text("Aperture: f/%.1f", g_image.metadata.aperture);
                ImGui::Text("Focal Length: %.1f mm", g_image.metadata.focal_length);
                ImGui::Separator();
                ImGui::Text("Date: %s", g_image.metadata.timestamp);
                if (g_image.metadata.artist[0] != '\0') {
                    ImGui::Text("Artist: %s", g_image.metadata.artist);
                }
            }

            ImGui::End();
        }

        if (params_changed) {
            if (ImGui::IsAnyItemActive()) {
                is_interacting_now = true;
                g_full_refresh_requested = false;
            } else {
                g_full_refresh_requested = true;
            }
            history_dirty = true;
            sidecar_dirty = true;
            should_request_processing = true;
            debug_log(
                "ui params_changed exp=%.2f contrast=%.2f temp=%.0f tint=%.0f sat=%.2f vib=%.2f lod=%d zoom=%.3f full_refresh=%d",
                g_params.exposure,
                g_params.contrast,
                g_params.temperature,
                g_params.tint,
                g_params.saturation,
                g_params.vibrance,
                g_image.lod_strength,
                g_image.zoom,
                g_full_refresh_requested.load() ? 1 : 0);
        }

        auto commit_history_snapshot = [&]() {
            if (!params_equal(history[(size_t)history_index], g_params)) {
                if (history_index + 1 < (int)history.size()) {
                    history.resize((size_t)history_index + 1);
                }
                history.push_back(g_params);
                history_index = (int)history.size() - 1;
            }
            history_dirty = false;
        };

        bool any_widget_active_after = ImGui::IsAnyItemActive();
        if (any_widget_active_after) is_interacting_now = true;
        g_interaction_active = is_interacting_now;

        if (g_turbo_preview_enabled.load() && was_interacting_last_frame && !is_interacting_now && g_image.proxy_buffer != NULL) {
            g_full_refresh_requested = true;
            should_request_processing = true;
        }
        was_interacting_last_frame = is_interacting_now;

        if (history_dirty && was_any_widget_active && !any_widget_active_after) {
            commit_history_snapshot();
        }
        if (history_dirty && !was_any_widget_active && !any_widget_active_after) {
            commit_history_snapshot();
        }
        was_any_widget_active = any_widget_active_after;

        bool ctrl_down = ImGui::GetIO().KeyCtrl;
        float keyboard_view_width = ImGui::GetContentRegionAvail().x;
        float keyboard_1x_zoom = 1.0f;
        float keyboard_2x_zoom = 2.0f;
        if (keyboard_view_width > 1.0f && g_image.width > 0) {
            keyboard_1x_zoom = (float)g_image.width / keyboard_view_width;
            keyboard_2x_zoom = keyboard_1x_zoom * 2.0f;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F)) {
            g_image.zoom = 1.0f;
            viewport_changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_1)) {
            g_image.zoom = keyboard_1x_zoom;
            viewport_changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_2)) {
            g_image.zoom = keyboard_2x_zoom;
            viewport_changed = true;
        }
        if (ctrl_down && ImGui::IsKeyPressed(ImGuiKey_Z) && history_index > 0) {
            history_index--;
            g_params = history[(size_t)history_index];
            g_full_refresh_requested = true;
            should_request_processing = true;
            history_dirty = false;
        }
        if (ctrl_down && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z) && history_index + 1 < (int)history.size()) {
            history_index++;
            g_params = history[(size_t)history_index];
            g_full_refresh_requested = true;
            should_request_processing = true;
            history_dirty = false;
        }
        if (ctrl_down && ImGui::IsKeyPressed(ImGuiKey_Y) && history_index + 1 < (int)history.size()) {
            history_index++;
            g_params = history[(size_t)history_index];
            g_full_refresh_requested = true;
            should_request_processing = true;
            history_dirty = false;
        }
        if (viewport_changed) {
            should_request_processing = true;
        }

        if (should_request_processing) {
            debug_log("ui request_processing -> seq_before=%llu full_refresh=%d", (unsigned long long)g_request_seq.load(std::memory_order_relaxed), g_full_refresh_requested.load() ? 1 : 0);
            request_processing();
            processing_pending = true;
            pending_request_seq = g_request_seq.load(std::memory_order_relaxed);
            pending_start_time = std::chrono::steady_clock::now();
        }

        if (g_processing_complete.exchange(false)) {
            if (processing_pending) {
                auto now = std::chrono::steady_clock::now();
                last_processing_ms = std::chrono::duration<float, std::milli>(now - pending_start_time).count();
                debug_log("ui processing complete seq=%llu elapsed=%.2fms current_seq=%llu", (unsigned long long)pending_request_seq, last_processing_ms, (unsigned long long)g_request_seq.load(std::memory_order_relaxed));
            }
            processing_pending = (g_request_seq.load(std::memory_order_relaxed) != pending_request_seq);

            // Upload to GPU (Main thread only)
            std::lock_guard<std::mutex> lock(g_buffer_mutex);
            glBindTexture(GL_TEXTURE_2D, g_image.texture_id);
            if (g_image.texture_width != g_image.width || g_image.texture_height != g_image.height) {
                auto tex_upload_begin = std::chrono::steady_clock::now();
                size_t bytes = (size_t)g_image.width * (size_t)g_image.height * 3u * sizeof(float);
                debug_log("ui gpu upload full texture size=%dx%d bytes=%.1fMB", g_image.width, g_image.height, (double)bytes / (1024.0 * 1024.0));
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, g_image.width, g_image.height, 0, GL_RGB, GL_FLOAT, g_image.working_buffer);
                auto tex_upload_end = std::chrono::steady_clock::now();
                float tex_upload_ms = std::chrono::duration<float, std::milli>(tex_upload_end - tex_upload_begin).count();
                debug_log("ui gpu upload full texture done ms=%.2f", tex_upload_ms);
                g_image.texture_width = g_image.width;
                g_image.texture_height = g_image.height;
            } else {
                int upload_x0 = g_completed_x_start.load();
                int upload_y0 = g_completed_y_start.load();
                int upload_x1 = g_completed_x_end.load();
                int upload_y1 = g_completed_y_end.load();
                if (upload_x0 < 0) upload_x0 = 0;
                if (upload_y0 < 0) upload_y0 = 0;
                if (upload_x1 > g_image.width) upload_x1 = g_image.width;
                if (upload_y1 > g_image.height) upload_y1 = g_image.height;
                if (upload_x1 <= upload_x0 || upload_y1 <= upload_y0) {
                    upload_x0 = 0;
                    upload_y0 = 0;
                    upload_x1 = g_image.width;
                    upload_y1 = g_image.height;
                }

                debug_log(
                    "ui gpu upload sub region=%d,%d -> %d,%d texture=%dx%d",
                    upload_x0,
                    upload_y0,
                    upload_x1,
                    upload_y1,
                    g_image.width,
                    g_image.height);

                glPixelStorei(GL_UNPACK_ROW_LENGTH, g_image.width);
                glTexSubImage2D(GL_TEXTURE_2D, 0, upload_x0, upload_y0,
                                upload_x1 - upload_x0,
                                upload_y1 - upload_y0,
                                GL_RGB, GL_FLOAT, g_image.working_buffer + (upload_y0 * g_image.width + upload_x0) * 3);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }

            bool need_original_texture_now = false;
            if (compare_mode == 2) {
                need_original_texture_now = true;
            } else if (compare_mode == 1 && ImGui::IsKeyDown(ImGuiKey_Space)) {
                need_original_texture_now = true;
            }

            if ((g_image.original_texture_width != g_image.width || g_image.original_texture_height != g_image.height) && need_original_texture_now) {
                auto orig_build_begin = std::chrono::steady_clock::now();
                const float* src = g_image.using_proxy_source ? g_image.proxy_buffer : g_image.full_buffer;
                int count = g_image.width * g_image.height * 3;
                std::vector<float> original_display_buffer((size_t)count);
                for (int i = 0; i < count; ++i) {
                    original_display_buffer[(size_t)i] = srgb_encode_display(src[i]);
                }

                glBindTexture(GL_TEXTURE_2D, g_image.original_texture_id);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, g_image.width, g_image.height, 0, GL_RGB, GL_FLOAT, original_display_buffer.data());
                g_image.original_texture_width = g_image.width;
                g_image.original_texture_height = g_image.height;
                auto orig_build_end = std::chrono::steady_clock::now();
                float orig_build_ms = std::chrono::duration<float, std::milli>(orig_build_end - orig_build_begin).count();
                debug_log("ui original texture rebuild done size=%dx%d ms=%.2f", g_image.width, g_image.height, orig_build_ms);
            } else if (g_image.original_texture_width != g_image.width || g_image.original_texture_height != g_image.height) {
                debug_log("ui original texture rebuild deferred compare_mode=%d", compare_mode);
            }
        }

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    g_keep_running = false;
    g_request_cv.notify_one();
    notify_thumbnail_worker_shutdown();
    worker.join();
    thumb_worker.join();
    
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    if (g_image.proxy_buffer) free(g_image.proxy_buffer);
    if (g_image.full_buffer) free(g_image.full_buffer);
    if (g_image.proxy_working_buffer) free(g_image.proxy_working_buffer);
    if (g_image.full_working_buffer) free(g_image.full_working_buffer);
    if (g_image.texture_id != 0) glDeleteTextures(1, &g_image.texture_id);
    if (g_image.original_texture_id != 0) glDeleteTextures(1, &g_image.original_texture_id);
    if (startup_icon_texture != 0) glDeleteTextures(1, &startup_icon_texture);

    return 0;
}