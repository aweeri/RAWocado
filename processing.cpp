#include "processing.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <chrono>
#include <vector>
#include <algorithm>
#include <string>
#include <cstdarg>
#include <deque>
#include <unordered_set>

#include <libraw/libraw.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#ifdef _WIN32
#include <initguid.h>
#include <objbase.h>
#include <wincodec.h>
#endif

// -----------------------------------------------------------------------------
// Global Instances
// -----------------------------------------------------------------------------
static ImageState make_default_image_state() {
    ImageState s = {};
    s.zoom = 1.0f;
    s.image_temperature = 5000.0f;
    s.show_r = true;
    s.show_g = true;
    s.show_b = true;
    s.show_luma = true;
    s.lod_strength = 3;
    s.histogram_quality = 8;
    s.histogram_interval_ms = 120;
    s.last_zoom = 1.0f;
    s.using_proxy_source = true;
    return s;
}

ImageState g_image = make_default_image_state();
EditorParams g_params = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 5000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
const EditorParams g_default_params = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 5000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

std::atomic<bool> g_processing_complete(false);
std::atomic<bool> g_keep_running(true);
std::atomic<bool> g_interaction_active(false);
std::atomic<bool> g_turbo_preview_enabled(true);
std::atomic<bool> g_export_in_progress(false);
std::atomic<bool> g_export_done(false);
std::atomic<bool> g_export_success(false);
std::atomic<float> g_export_progress(0.0f);
std::atomic<bool> g_load_in_progress(false);
std::atomic<bool> g_load_done(false);
std::atomic<bool> g_load_success(false);
std::atomic<float> g_load_progress(0.0f);
std::atomic<float> g_last_process_ms(0.0f);
std::atomic<float> g_last_histogram_ms(0.0f);
std::atomic<int> g_current_processing_step(1);
std::atomic<float> g_current_effective_decimation(1.0f);
std::atomic<int> g_current_adaptive_pixels(0);
std::atomic<bool> g_full_refresh_requested(false);
std::atomic<int> g_completed_x_start(0);
std::atomic<int> g_completed_y_start(0);
std::atomic<int> g_completed_x_end(0);
std::atomic<int> g_completed_y_end(0);
std::mutex g_buffer_mutex;
std::mutex g_export_mutex;
std::mutex g_request_mutex;
std::condition_variable g_request_cv;
std::atomic<uint64_t> g_request_seq(0);

static const float kMinTempK = 2000.0f;
static const float kMaxTempK = 15000.0f;
static std::chrono::steady_clock::time_point g_last_histogram_compute = std::chrono::steady_clock::now() - std::chrono::milliseconds(1000);
char g_current_image_path[1024] = {0};
static EditorParams g_last_processed_params = {};
static bool g_has_last_processed_params = false;

static std::mutex g_thumbnail_mutex;
static std::condition_variable g_thumbnail_cv;
static std::deque<std::string> g_thumbnail_queue;
static std::unordered_set<std::string> g_thumbnail_seen;
static std::deque<ThumbnailPreview> g_thumbnail_ready_queue;

static bool convert_bitmap_thumb_to_rgba(const libraw_processed_image_t* thumb, ThumbnailPreview* out_preview) {
    if (!thumb || !out_preview || !thumb->data || thumb->width <= 0 || thumb->height <= 0) return false;

    const int pixels = thumb->width * thumb->height;
    const int channels = (thumb->colors >= 1 && thumb->colors <= 4) ? thumb->colors : 3;
    out_preview->width = thumb->width;
    out_preview->height = thumb->height;
    out_preview->rgba_pixels.resize((size_t)pixels * 4u);

    const uint8_t* src = thumb->data;
    for (int i = 0; i < pixels; ++i) {
        uint8_t r = 0, g = 0, b = 0, a = 255;
        if (channels == 1) {
            r = g = b = src[i];
        } else if (channels == 3) {
            r = src[i * 3 + 0];
            g = src[i * 3 + 1];
            b = src[i * 3 + 2];
        } else {
            r = src[i * 4 + 0];
            g = src[i * 4 + 1];
            b = src[i * 4 + 2];
            a = src[i * 4 + 3];
        }

        size_t dst = (size_t)i * 4u;
        out_preview->rgba_pixels[dst + 0] = r;
        out_preview->rgba_pixels[dst + 1] = g;
        out_preview->rgba_pixels[dst + 2] = b;
        out_preview->rgba_pixels[dst + 3] = a;
    }

    return true;
}

#ifdef _WIN32
static bool decode_jpeg_thumb_with_wic(const uint8_t* data, size_t size, ThumbnailPreview* out_preview) {
    if (!data || size == 0 || !out_preview) return false;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool should_uninit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) {
        should_uninit = false;
    } else if (FAILED(hr)) {
        return false;
    }

    IWICImagingFactory* factory = NULL;
    IWICStream* stream = NULL;
    IWICBitmapDecoder* decoder = NULL;
    IWICBitmapFrameDecode* frame = NULL;
    IWICFormatConverter* converter = NULL;
    bool ok = false;

    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) break;
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromMemory((BYTE*)data, (DWORD)size))) break;
        if (FAILED(factory->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) break;

        UINT width = 0;
        UINT height = 0;
        if (FAILED(converter->GetSize(&width, &height))) break;
        if (width == 0 || height == 0) break;

        out_preview->width = (int)width;
        out_preview->height = (int)height;
        out_preview->rgba_pixels.resize((size_t)width * (size_t)height * 4u);
        if (FAILED(converter->CopyPixels(NULL, width * 4u, (UINT)out_preview->rgba_pixels.size(), out_preview->rgba_pixels.data()))) break;

        ok = true;
    } while (false);

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (should_uninit) CoUninitialize();
    return ok;
}
#endif

static bool decode_raw_thumbnail(const char* filepath, ThumbnailPreview* out_preview) {
    if (!filepath || !filepath[0] || !out_preview) return false;

    libraw_data_t* processor = libraw_init(0);
    if (!processor) return false;

    bool ok = false;
    int err = 0;
    do {
        if (libraw_open_file(processor, filepath) != LIBRAW_SUCCESS) break;
        if (libraw_unpack_thumb(processor) != LIBRAW_SUCCESS) break;

        libraw_processed_image_t* thumb = libraw_dcraw_make_mem_thumb(processor, &err);
        if (!thumb) break;

        snprintf(out_preview->path, sizeof(out_preview->path), "%s", filepath);
        if (thumb->type == LIBRAW_IMAGE_BITMAP) {
            ok = convert_bitmap_thumb_to_rgba(thumb, out_preview);
        } else if (thumb->type == LIBRAW_IMAGE_JPEG) {
#ifdef _WIN32
            ok = decode_jpeg_thumb_with_wic(thumb->data, thumb->data_size, out_preview);
#endif
        }

        libraw_dcraw_clear_mem(thumb);
    } while (false);

    libraw_close(processor);
    return ok;
}

static float load_stage_base(enum LibRaw_progress stage) {
    switch (stage) {
    case LIBRAW_PROGRESS_START: return 0.01f;
    case LIBRAW_PROGRESS_OPEN: return 0.05f;
    case LIBRAW_PROGRESS_IDENTIFY: return 0.12f;
    case LIBRAW_PROGRESS_SIZE_ADJUST: return 0.20f;
    case LIBRAW_PROGRESS_LOAD_RAW: return 0.35f;
    case LIBRAW_PROGRESS_RAW2_IMAGE: return 0.62f;
    case LIBRAW_PROGRESS_REMOVE_ZEROES: return 0.70f;
    case LIBRAW_PROGRESS_BAD_PIXELS: return 0.74f;
    case LIBRAW_PROGRESS_DARK_FRAME: return 0.78f;
    case LIBRAW_PROGRESS_FOVEON_INTERPOLATE: return 0.82f;
    case LIBRAW_PROGRESS_SCALE_COLORS: return 0.86f;
    case LIBRAW_PROGRESS_PRE_INTERPOLATE: return 0.90f;
    case LIBRAW_PROGRESS_INTERPOLATE: return 0.93f;
    default: return 0.0f;
    }
}

static int load_progress_callback(void* /*data*/, enum LibRaw_progress stage, int iteration, int expected) {
    float base = load_stage_base(stage);
    float span = 0.0f;
    switch (stage) {
    case LIBRAW_PROGRESS_START:
    case LIBRAW_PROGRESS_OPEN:
    case LIBRAW_PROGRESS_IDENTIFY:
    case LIBRAW_PROGRESS_SIZE_ADJUST:
    case LIBRAW_PROGRESS_LOAD_RAW:
    case LIBRAW_PROGRESS_RAW2_IMAGE:
    case LIBRAW_PROGRESS_REMOVE_ZEROES:
    case LIBRAW_PROGRESS_BAD_PIXELS:
    case LIBRAW_PROGRESS_DARK_FRAME:
    case LIBRAW_PROGRESS_FOVEON_INTERPOLATE:
    case LIBRAW_PROGRESS_SCALE_COLORS:
    case LIBRAW_PROGRESS_PRE_INTERPOLATE:
    case LIBRAW_PROGRESS_INTERPOLATE:
        span = 0.04f;
        break;
    default:
        span = 0.0f;
        break;
    }

    float local = 0.0f;
    if (expected > 0) {
        local = (float)iteration / (float)expected;
        if (local < 0.0f) local = 0.0f;
        if (local > 1.0f) local = 1.0f;
    }

    float progress = base + span * local;
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 0.98f) progress = 0.98f;
    g_load_progress = progress;
    return 0;
}

void debug_log(const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    unsigned long long thread_id = (unsigned long long)std::hash<std::thread::id>{}(std::this_thread::get_id());
    fprintf(stderr, "[%.3f][tid=%llu] %s\n", seconds, thread_id, message);
    fflush(stderr);
}

// -----------------------------------------------------------------------------
// Core Processing Math
// -----------------------------------------------------------------------------
inline float clamp(float x, float min_val, float max_val) {
    return (x < min_val) ? min_val : (x > max_val) ? max_val : x;
}

static inline float mixf(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline float luminance709(float r, float g, float b) {
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

static inline float srgb_encode(float v) {
    v = clamp(v, 0.0f, 1.0f);
    if (v <= 0.0031308f) return 12.92f * v;
    return 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static inline float srgb_encode_fast(float v) {
    const int LUT_SIZE = 4096;
    static bool lut_ready = false;
    static float lut[LUT_SIZE + 1];

    if (!lut_ready) {
        for (int i = 0; i <= LUT_SIZE; ++i) {
            float x = (float)i / (float)LUT_SIZE;
            lut[i] = srgb_encode(x);
        }
        lut_ready = true;
    }

    v = clamp(v, 0.0f, 1.0f);
    float f = v * (float)LUT_SIZE;
    int i0 = (int)f;
    int i1 = (i0 < LUT_SIZE) ? (i0 + 1) : LUT_SIZE;
    float t = f - (float)i0;
    return mixf(lut[i0], lut[i1], t);
}

static inline float srgb_decode_fast(float v) {
    v = clamp(v, 0.0f, 1.0f);
    if (v <= 0.04045f) return v / 12.92f;
    return powf((v + 0.055f) / 1.055f, 2.4f);
}

static bool params_equal_except_exposure(const EditorParams& a, const EditorParams& b, float eps = 1e-5f) {
    const float* pa = (const float*)&a;
    const float* pb = (const float*)&b;
    int count = (int)(sizeof(EditorParams) / sizeof(float));
    for (int i = 0; i < count; ++i) {
        if (i == 0) continue; // exposure
        if (fabsf(pa[i] - pb[i]) > eps) return false;
    }
    return true;
}

static void rebuild_histogram_from_buffer(const float* buffer, int width, int height, HistogramData& histogram) {
    memset(&histogram, 0, sizeof(HistogramData));
    if (!buffer || width <= 0 || height <= 0) return;

    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 4;
    int threads = (int)std::min<unsigned int>(hw_threads, (unsigned int)std::max(height, 1));

    if (threads <= 1) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int idx = (y * width + x) * 3;
                float r = buffer[idx];
                float g = buffer[idx + 1];
                float b = buffer[idx + 2];
                int bin_r = (int)(clamp(r, 0.0f, 1.0f) * 255.0f);
                int bin_g = (int)(clamp(g, 0.0f, 1.0f) * 255.0f);
                int bin_b = (int)(clamp(b, 0.0f, 1.0f) * 255.0f);
                float luma = 0.299f * r + 0.587f * g + 0.114f * b;
                int bin_luma = (int)(clamp(luma, 0.0f, 1.0f) * 255.0f);
                histogram.r[bin_r]++;
                histogram.g[bin_g]++;
                histogram.b[bin_b]++;
                histogram.luma[bin_luma]++;
            }
        }
    } else {
        std::vector<HistogramData> partial((size_t)threads);
        auto worker = [&](int y0, int y1, HistogramData& h) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < width; ++x) {
                    int idx = (y * width + x) * 3;
                    float r = buffer[idx];
                    float g = buffer[idx + 1];
                    float b = buffer[idx + 2];
                    int bin_r = (int)(clamp(r, 0.0f, 1.0f) * 255.0f);
                    int bin_g = (int)(clamp(g, 0.0f, 1.0f) * 255.0f);
                    int bin_b = (int)(clamp(b, 0.0f, 1.0f) * 255.0f);
                    float luma = 0.299f * r + 0.587f * g + 0.114f * b;
                    int bin_luma = (int)(clamp(luma, 0.0f, 1.0f) * 255.0f);
                    h.r[bin_r]++;
                    h.g[bin_g]++;
                    h.b[bin_b]++;
                    h.luma[bin_luma]++;
                }
            }
        };

        int chunk = (height + threads - 1) / threads;
        std::vector<std::thread> workers;
        workers.reserve((size_t)threads - 1);
        for (int t = 0; t < threads - 1; ++t) {
            int y0 = t * chunk;
            int y1 = std::min(height, y0 + chunk);
            if (y0 < y1) workers.emplace_back(worker, y0, y1, std::ref(partial[(size_t)t]));
        }
        int my_y0 = (threads - 1) * chunk;
        if (my_y0 < height) worker(my_y0, height, partial[(size_t)threads - 1]);
        for (auto& thread : workers) thread.join();

        for (int t = 0; t < threads; ++t) {
            for (int i = 0; i < 256; ++i) {
                histogram.r[i] += partial[(size_t)t].r[i];
                histogram.g[i] += partial[(size_t)t].g[i];
                histogram.b[i] += partial[(size_t)t].b[i];
                histogram.luma[i] += partial[(size_t)t].luma[i];
            }
        }
    }

    histogram.max_val = 0.0f;
    for (int i = 0; i < 256; ++i) {
        if (histogram.r[i] > histogram.max_val) histogram.max_val = histogram.r[i];
        if (histogram.g[i] > histogram.max_val) histogram.max_val = histogram.g[i];
        if (histogram.b[i] > histogram.max_val) histogram.max_val = histogram.b[i];
        if (histogram.luma[i] > histogram.max_val) histogram.max_val = histogram.luma[i];
    }
}

static inline float smoothstep01(float x) {
    x = clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static inline float smoothstep(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    return smoothstep01(t);
}

static inline float inverse_smoothstep(float x) {
    x = clamp(x, 0.0f, 1.0f);
    return 0.5f - sinf(asinf(1.0f - 2.0f * x) / 3.0f);
}

static inline float soft_slider(float v, float gamma) {
    float n = clamp(v / 100.0f, -1.0f, 1.0f);
    float a = powf(fabsf(n), gamma);
    return (n < 0.0f) ? -a : a;
}

static void mat3_mul(const float a[9], const float b[9], float out[9]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 3 + c] =
                a[r * 3 + 0] * b[0 * 3 + c] +
                a[r * 3 + 1] * b[1 * 3 + c] +
                a[r * 3 + 2] * b[2 * 3 + c];
        }
    }
}

static void mat3_vec_mul(const float m[9], float x, float y, float z, float* ox, float* oy, float* oz) {
    *ox = m[0] * x + m[1] * y + m[2] * z;
    *oy = m[3] * x + m[4] * y + m[5] * z;
    *oz = m[6] * x + m[7] * y + m[8] * z;
}

static void kelvin_to_xy(float kelvin, float* x, float* y) {
    float t = clamp(kelvin, 1000.0f, 40000.0f);
    float x_c;
    if (t <= 4000.0f) {
        x_c = -0.2661239e9f / (t * t * t) - 0.2343589e6f / (t * t) + 0.8776956e3f / t + 0.179910f;
    } else {
        x_c = -3.0258469e9f / (t * t * t) + 2.1070379e6f / (t * t) + 0.2226347e3f / t + 0.240390f;
    }

    float y_c;
    if (t <= 2222.0f) {
        y_c = -1.1063814f * x_c * x_c * x_c - 1.34811020f * x_c * x_c + 2.18555832f * x_c - 0.20219683f;
    } else if (t <= 4000.0f) {
        y_c = -0.9549476f * x_c * x_c * x_c - 1.37418593f * x_c * x_c + 2.09137015f * x_c - 0.16748867f;
    } else {
        y_c = 3.0817580f * x_c * x_c * x_c - 5.87338670f * x_c * x_c + 3.75112997f * x_c - 0.37001483f;
    }

    *x = x_c;
    *y = y_c;
}

static void kelvin_to_xyz(float kelvin, float* X, float* Y, float* Z) {
    float x, y;
    kelvin_to_xy(kelvin, &x, &y);
    *Y = 1.0f;
    *X = x / y;
    *Z = (1.0f - x - y) / y;
}

struct ProcessingContext {
    EditorParams params;
    int width;
    int height;
    int stage_x0;
    int stage_y0;
    int stage_x1;
    int stage_y1;
    int stage_w;
    int stage_h;
    const float* src_buffer;
    float exp_mult;
    float wb_rgb[9];
    float clarity_amt;
    float texture_amt;
    float dehaze_amt;
    float saturation;
    float vibrance;
    float nr_amt;
    float cnr_amt;
    bool use_spatial;
    bool use_dehaze;
    bool use_nr;
    bool use_cnr;
    bool has_ycbcr_maps;
    std::vector<float> staged_rgb;
    std::vector<float> luma;
    std::vector<float> cb;
    std::vector<float> cr;
    std::vector<float> blur_small;
    std::vector<float> blur_large;
    std::vector<float> dark_blur;
    std::vector<float> luma_nr;
    std::vector<float> cb_nr;
    std::vector<float> cr_nr;
    float atmosphere;
    float atmosphere_rgb[3];
};

static const float* g_active_source_buffer = NULL;

static float median_window(const std::vector<float>& src, int w, int h, int x, int y, int radius);

static void box_blur_1d_h(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int r) {
    if (r <= 0) {
        dst = src;
        return;
    }
    dst.assign((size_t)w * h, 0.0f);
    int ksize = 2 * r + 1;

    for (int y = 0; y < h; ++y) {
        float sum = 0.0f;
        int row = y * w;
        for (int k = -r; k <= r; ++k) {
            int x = k;
            if (x < 0) x = 0;
            if (x >= w) x = w - 1;
            sum += src[row + x];
        }
        dst[row] = sum / (float)ksize;

        for (int x = 1; x < w; ++x) {
            int add_x = x + r;
            int sub_x = x - r - 1;
            if (add_x >= w) add_x = w - 1;
            if (sub_x < 0) sub_x = 0;
            sum += src[row + add_x] - src[row + sub_x];
            dst[row + x] = sum / (float)ksize;
        }
    }
}

static void box_blur_1d_v(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int r) {
    if (r <= 0) {
        dst = src;
        return;
    }
    dst.assign((size_t)w * h, 0.0f);
    int ksize = 2 * r + 1;

    for (int x = 0; x < w; ++x) {
        float sum = 0.0f;
        for (int k = -r; k <= r; ++k) {
            int y = k;
            if (y < 0) y = 0;
            if (y >= h) y = h - 1;
            sum += src[y * w + x];
        }
        dst[x] = sum / (float)ksize;

        for (int y = 1; y < h; ++y) {
            int add_y = y + r;
            int sub_y = y - r - 1;
            if (add_y >= h) add_y = h - 1;
            if (sub_y < 0) sub_y = 0;
            sum += src[add_y * w + x] - src[sub_y * w + x];
            dst[y * w + x] = sum / (float)ksize;
        }
    }
}

static void box_blur(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int radius) {
    std::vector<float> tmp;
    box_blur_1d_h(src, tmp, w, h, radius);
    box_blur_1d_v(tmp, dst, w, h, radius);
}

static void median_filter_map(const std::vector<float>& src, std::vector<float>& dst, int w, int h, int radius) {
    dst.resize((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            dst[(size_t)y * w + x] = median_window(src, w, h, x, y, radius);
        }
    }
}

static void build_luma_nr_map(const std::vector<float>& luma, std::vector<float>& out_nr, int w, int h, float nr_amt) {
    int radius = (nr_amt > 0.66f) ? 3 : ((nr_amt > 0.33f) ? 2 : 1);
    std::vector<float> blurred;
    box_blur(luma, blurred, w, h, radius);

    out_nr.resize((size_t)w * h);
    for (int i = 0; i < w * h; ++i) {
        float y = luma[i];
        float b = blurred[i];
        float diff = fabsf(y - b);
        float thresh = 0.01f + 0.08f * clamp(y, 0.0f, 1.0f);
        float edge = clamp(diff / (thresh + 1e-6f), 0.0f, 1.0f);
        float blend = nr_amt * (1.0f - edge);
        out_nr[i] = mixf(y, b, blend);
    }
}

static void build_wb_matrix(float source_kelvin, float target_kelvin, float tint, float out_rgb_matrix[9]) {
    const float rgb_to_xyz[9] = {
        0.4124564f, 0.3575761f, 0.1804375f,
        0.2126729f, 0.7151522f, 0.0721750f,
        0.0193339f, 0.1191920f, 0.9503041f
    };
    const float xyz_to_rgb[9] = {
        3.2404542f, -1.5371385f, -0.4985314f,
        -0.9692660f, 1.8760108f, 0.0415560f,
        0.0556434f, -0.2040259f, 1.0572252f
    };

    const float bradford[9] = {
        0.8951f, 0.2664f, -0.1614f,
        -0.7502f, 1.7135f, 0.0367f,
        0.0389f, -0.0685f, 1.0296f
    };
    const float bradford_inv[9] = {
        0.9869929f, -0.1470543f, 0.1599627f,
        0.4323053f, 0.5183603f, 0.0492912f,
        -0.0085287f, 0.0400428f, 0.9684867f
    };

    float src_X, src_Y, src_Z;
    float dst_X, dst_Y, dst_Z;
    kelvin_to_xyz(source_kelvin, &src_X, &src_Y, &src_Z);
    kelvin_to_xyz(target_kelvin, &dst_X, &dst_Y, &dst_Z);

    float src_l, src_m, src_s;
    float dst_l, dst_m, dst_s;
    mat3_vec_mul(bradford, src_X, src_Y, src_Z, &src_l, &src_m, &src_s);
    mat3_vec_mul(bradford, dst_X, dst_Y, dst_Z, &dst_l, &dst_m, &dst_s);

    float tint_n = clamp(tint / 150.0f, -1.0f, 1.0f);
    dst_l *= (1.0f + 0.10f * tint_n);
    dst_m *= (1.0f - 0.10f * tint_n);

    float diag[9] = {
        dst_l / (src_l + 1e-8f), 0.0f, 0.0f,
        0.0f, dst_m / (src_m + 1e-8f), 0.0f,
        0.0f, 0.0f, dst_s / (src_s + 1e-8f)
    };

    float tmp0[9];
    float adapt_xyz[9];
    mat3_mul(diag, bradford, tmp0);
    mat3_mul(bradford_inv, tmp0, adapt_xyz);

    float tmp1[9];
    mat3_mul(adapt_xyz, rgb_to_xyz, tmp1);
    mat3_mul(xyz_to_rgb, tmp1, out_rgb_matrix);
}

static inline void rgb_to_ycbcr(float r, float g, float b, float* y, float* cb, float* cr) {
    *y = luminance709(r, g, b);
    *cb = (b - *y) / 1.8556f;
    *cr = (r - *y) / 1.5748f;
}

static inline void ycbcr_to_rgb(float y, float cb, float cr, float* r, float* g, float* b) {
    *r = y + 1.5748f * cr;
    *b = y + 1.8556f * cb;
    *g = (y - 0.2126f * (*r) - 0.0722f * (*b)) / 0.7152f;
}

static inline void apply_wb_and_exposure(const ProcessingContext& ctx, float r, float g, float b, float* out_r, float* out_g, float* out_b) {
    float wr, wg, wb;
    mat3_vec_mul(ctx.wb_rgb, r, g, b, &wr, &wg, &wb);
    *out_r = wr * ctx.exp_mult;
    *out_g = wg * ctx.exp_mult;
    *out_b = wb * ctx.exp_mult;
}

static inline int stage_index_clamped(const ProcessingContext& ctx, int x, int y) {
    int sx = x;
    int sy = y;
    if (sx < ctx.stage_x0) sx = ctx.stage_x0;
    if (sy < ctx.stage_y0) sy = ctx.stage_y0;
    if (sx >= ctx.stage_x1) sx = ctx.stage_x1 - 1;
    if (sy >= ctx.stage_y1) sy = ctx.stage_y1 - 1;
    return (sy - ctx.stage_y0) * ctx.stage_w + (sx - ctx.stage_x0);
}

static inline void sample_staged_rgb(const ProcessingContext& ctx, int x, int y, float* r, float* g, float* b) {
    int local_idx = stage_index_clamped(ctx, x, y);
    int rgb_idx = local_idx * 3;
    *r = ctx.staged_rgb[(size_t)rgb_idx];
    *g = ctx.staged_rgb[(size_t)rgb_idx + 1];
    *b = ctx.staged_rgb[(size_t)rgb_idx + 2];
}

static inline void sample_linear_ycbcr(const ProcessingContext& ctx, int x, int y, float* yv, float* cbv, float* crv) {
    int idx = stage_index_clamped(ctx, x, y);
    if (ctx.has_ycbcr_maps) {
        *yv = ctx.luma[(size_t)idx];
        *cbv = ctx.cb[(size_t)idx];
        *crv = ctx.cr[(size_t)idx];
        return;
    }

    int rgb_idx = idx * 3;
    rgb_to_ycbcr(
        ctx.staged_rgb[(size_t)rgb_idx],
        ctx.staged_rgb[(size_t)rgb_idx + 1],
        ctx.staged_rgb[(size_t)rgb_idx + 2],
        yv, cbv, crv);
}

static ProcessingContext build_processing_context_from_snapshot(bool fast_preview, const EditorParams& params,
    int width, int height, int vis_x0, int vis_y0, int vis_x1, int vis_y1,
    const float* source_buffer, float source_temperature) {
    ProcessingContext ctx = {};
    ctx.params = params;
    ctx.width = width;
    ctx.height = height;
    ctx.exp_mult = powf(2.0f, params.exposure);
    ctx.clarity_amt = params.clarity / 100.0f;
    ctx.texture_amt = params.texture / 100.0f;
    ctx.dehaze_amt = clamp(params.dehaze / 100.0f, -1.0f, 1.0f);
    ctx.saturation = 1.0f + (params.saturation / 100.0f);
    ctx.vibrance = params.vibrance / 100.0f;
    ctx.nr_amt = clamp(params.noise_reduction / 100.0f, 0.0f, 1.0f);
    ctx.cnr_amt = clamp(params.color_noise_reduction / 100.0f, 0.0f, 1.0f);
    if (fast_preview) {
        // Keep controls responsive in preview by disabling the heaviest kernels.
        ctx.clarity_amt *= 0.45f;
        ctx.texture_amt *= 0.45f;
        ctx.dehaze_amt = 0.0f;
        ctx.nr_amt = 0.0f;
        ctx.cnr_amt = 0.0f;
    }

    ctx.use_spatial = (fabsf(ctx.clarity_amt) > 1e-4f) || (fabsf(ctx.texture_amt) > 1e-4f);
    ctx.use_dehaze = (fabsf(ctx.dehaze_amt) > 1e-4f);
    ctx.use_nr = (ctx.nr_amt > 1e-4f);
    ctx.use_cnr = (ctx.cnr_amt > 1e-4f);
    ctx.has_ycbcr_maps = false;
    ctx.src_buffer = source_buffer;

    float src_temp = source_temperature;
    if (src_temp <= 1000.0f) src_temp = 5000.0f;
    build_wb_matrix(src_temp, clamp(params.temperature, kMinTempK, kMaxTempK), params.tint, ctx.wb_rgb);

    if (vis_x0 >= vis_x1 || vis_y0 >= vis_y1) {
        vis_x0 = 0;
        vis_y0 = 0;
        vis_x1 = ctx.width;
        vis_y1 = ctx.height;
    }

    int kernel_pad = 0;
    if (ctx.use_spatial) kernel_pad = std::max(kernel_pad, 6);
    if (ctx.use_dehaze) kernel_pad = std::max(kernel_pad, 8);
    if (ctx.use_nr) kernel_pad = std::max(kernel_pad, (int)ceilf(1.0f + 2.0f * ctx.nr_amt));
    if (ctx.use_cnr) kernel_pad = std::max(kernel_pad, (int)ceilf(1.0f + ctx.cnr_amt));

    ctx.stage_x0 = vis_x0 - kernel_pad;
    ctx.stage_y0 = vis_y0 - kernel_pad;
    ctx.stage_x1 = vis_x1 + kernel_pad;
    ctx.stage_y1 = vis_y1 + kernel_pad;
    if (ctx.stage_x0 < 0) ctx.stage_x0 = 0;
    if (ctx.stage_y0 < 0) ctx.stage_y0 = 0;
    if (ctx.stage_x1 > ctx.width) ctx.stage_x1 = ctx.width;
    if (ctx.stage_y1 > ctx.height) ctx.stage_y1 = ctx.height;

    ctx.stage_w = ctx.stage_x1 - ctx.stage_x0;
    ctx.stage_h = ctx.stage_y1 - ctx.stage_y0;
    if (ctx.stage_w <= 0 || ctx.stage_h <= 0) {
        ctx.stage_x0 = 0;
        ctx.stage_y0 = 0;
        ctx.stage_x1 = ctx.width;
        ctx.stage_y1 = ctx.height;
        ctx.stage_w = ctx.width;
        ctx.stage_h = ctx.height;
    }

    size_t n = (size_t)ctx.stage_w * (size_t)ctx.stage_h;
    ctx.staged_rgb.resize(n * 3);

    bool need_luma = ctx.use_spatial || ctx.use_dehaze || ctx.use_nr || ctx.use_cnr;
    bool need_chroma = ctx.use_nr || ctx.use_cnr;
    bool need_dark = ctx.use_dehaze;

    if (need_luma) ctx.luma.resize(n);
    if (need_chroma) {
        ctx.cb.resize(n);
        ctx.cr.resize(n);
        ctx.has_ycbcr_maps = true;
    }
    std::vector<float> dark;
    if (need_dark) dark.resize(n);

    float atmosphere = 0.0f;
    float top_dark = -1.0f;
    float top_r = 1.0f, top_g = 1.0f, top_b = 1.0f;
    if (ctx.src_buffer && n > 0) {
        for (int y = ctx.stage_y0; y < ctx.stage_y1; ++y) {
            for (int x = ctx.stage_x0; x < ctx.stage_x1; ++x) {
                int src_idx = (y * ctx.width + x) * 3;
                int local_idx = (y - ctx.stage_y0) * ctx.stage_w + (x - ctx.stage_x0);
                float r, g, b;
                apply_wb_and_exposure(ctx,
                    ctx.src_buffer[src_idx],
                    ctx.src_buffer[src_idx + 1],
                    ctx.src_buffer[src_idx + 2],
                    &r, &g, &b);

                int rgb_idx = local_idx * 3;
                ctx.staged_rgb[(size_t)rgb_idx] = r;
                ctx.staged_rgb[(size_t)rgb_idx + 1] = g;
                ctx.staged_rgb[(size_t)rgb_idx + 2] = b;

                if (need_luma || need_chroma) {
                    float yv, cbv, crv;
                    rgb_to_ycbcr(r, g, b, &yv, &cbv, &crv);
                    if (need_luma) ctx.luma[(size_t)local_idx] = yv;
                    if (need_chroma) {
                        ctx.cb[(size_t)local_idx] = cbv;
                        ctx.cr[(size_t)local_idx] = crv;
                    }
                }

                if (need_dark) {
                    dark[(size_t)local_idx] = fminf(r, fminf(g, b));
                    float local_max = fmaxf(r, fmaxf(g, b));
                    if (local_max > atmosphere) atmosphere = local_max;
                    if (dark[(size_t)local_idx] > top_dark) {
                        top_dark = dark[(size_t)local_idx];
                        top_r = r;
                        top_g = g;
                        top_b = b;
                    }
                }
            }
        }
    }

    if (ctx.use_spatial) {
        box_blur(ctx.luma, ctx.blur_small, ctx.stage_w, ctx.stage_h, 2);
        box_blur(ctx.luma, ctx.blur_large, ctx.stage_w, ctx.stage_h, 6);
    }

    if (ctx.use_dehaze) {
        box_blur(dark, ctx.dark_blur, ctx.stage_w, ctx.stage_h, 8);
    }

    ctx.atmosphere = atmosphere;
    if (ctx.atmosphere < 1e-4f) ctx.atmosphere = 1.0f;
    ctx.atmosphere_rgb[0] = fmaxf(top_r, 1e-4f);
    ctx.atmosphere_rgb[1] = fmaxf(top_g, 1e-4f);
    ctx.atmosphere_rgb[2] = fmaxf(top_b, 1e-4f);
    return ctx;
}

static ProcessingContext build_processing_context(bool fast_preview, const EditorParams& params) {
    return build_processing_context_from_snapshot(
        fast_preview,
        params,
        g_image.width,
        g_image.height,
        g_image.vis_x_start,
        g_image.vis_y_start,
        g_image.vis_x_end,
        g_image.vis_y_end,
        g_active_source_buffer,
        g_image.image_temperature);
}

static float median_window(const std::vector<float>& src, int w, int h, int x, int y, int radius) {
    float vals[49];
    int n = 0;
    for (int ky = -radius; ky <= radius; ++ky) {
        int sy = y + ky;
        if (sy < 0) sy = 0;
        if (sy >= h) sy = h - 1;
        for (int kx = -radius; kx <= radius; ++kx) {
            int sx = x + kx;
            if (sx < 0) sx = 0;
            if (sx >= w) sx = w - 1;
            vals[n++] = src[sy * w + sx];
        }
    }
    int mid = n / 2;
    std::nth_element(vals, vals + mid, vals + n);
    return vals[mid];
}

static float apply_luma_tone(const ProcessingContext& ctx, float y) {
    y = fmaxf(y, 0.0f);
    float y_norm = clamp(y, 0.0f, 1.0f);

    float h = soft_slider(ctx.params.highlights, 1.20f);
    float s = soft_slider(ctx.params.shadows, 1.50f);
    float m = soft_slider(ctx.params.midtones, 1.25f);
    float w = soft_slider(ctx.params.whites, 1.25f);
    float b = soft_slider(ctx.params.blacks, 1.25f);

    float highlight_mask = smoothstep(0.42f, 0.98f, y_norm);
    float shadow_mask = 1.0f - smoothstep(0.02f, 0.62f, y_norm);
    float mid_mask = smoothstep(0.12f, 0.45f, y_norm) * (1.0f - smoothstep(0.55f, 0.90f, y_norm));
    float white_mask = smoothstep(0.68f, 1.0f, y_norm);
    float black_mask = 1.0f - smoothstep(0.0f, 0.32f, y_norm);

    float delta = 0.0f;
    delta += h * highlight_mask * 0.58f;
    delta += s * shadow_mask * 0.42f;
    delta += m * mid_mask * 0.42f;
    delta += w * white_mask * 0.68f;
    delta += b * black_mask * 0.68f;

    // Roll off near extreme ends so slider edges feel smoother and less abrupt.
    float highlight_rolloff = 1.0f - 0.35f * smoothstep(0.86f, 1.0f, y_norm);
    float shadow_rolloff = 1.0f - 0.30f * (1.0f - smoothstep(0.0f, 0.14f, y_norm));
    delta *= highlight_rolloff * shadow_rolloff;

    float out_y = y + delta;

    float contrast_delta = ctx.params.contrast / 100.0f;
    float yn = clamp(out_y, 0.0f, 1.0f);
    if (contrast_delta > 0.0f) {
        float scurve = smoothstep01(yn);
        yn = mixf(yn, scurve, clamp(contrast_delta, 0.0f, 1.0f));
    } else if (contrast_delta < 0.0f) {
        float invcurve = inverse_smoothstep(yn);
        yn = mixf(yn, invcurve, clamp(-contrast_delta, 0.0f, 1.0f));
    }

    return fmaxf(yn, 0.0f);
}

static void process_pixel(const ProcessingContext& ctx, int x, int y, float* out_r, float* out_g, float* out_b) {
    int idx = stage_index_clamped(ctx, x, y);

    float r, g, b;
    sample_staged_rgb(ctx, x, y, &r, &g, &b);

    if (ctx.use_dehaze) {
        float dehaze_amt = ctx.dehaze_amt;
        float A = ctx.atmosphere;
        float Ar = ctx.atmosphere_rgb[0];
        float Ag = ctx.atmosphere_rgb[1];
        float Ab = ctx.atmosphere_rgb[2];
        float t = 1.0f - 0.93f * (ctx.dark_blur[idx] / (A + 1e-6f));
        t = clamp(t, 0.2f, 1.0f);
        float luma_lin = clamp(luminance709(r, g, b), 0.0f, 1.0f);
        float highlight_protect = smoothstep(0.65f, 1.0f, luma_lin);
        float strength = dehaze_amt * (1.0f - 0.40f * highlight_protect);

        if (strength > 0.0f) {
            float jr = (r - Ar * (1.0f - t)) / t;
            float jg = (g - Ag * (1.0f - t)) / t;
            float jb = (b - Ab * (1.0f - t)) / t;
            r = mixf(r, jr, strength);
            g = mixf(g, jg, strength);
            b = mixf(b, jb, strength);
        } else {
            float haze_amt = -strength;
            float hr = r * t + Ar * (1.0f - t);
            float hg = g * t + Ag * (1.0f - t);
            float hb = b * t + Ab * (1.0f - t);
            r = mixf(r, hr, haze_amt);
            g = mixf(g, hg, haze_amt);
            b = mixf(b, hb, haze_amt);
        }
    }

    float y0 = luminance709(r, g, b);
    float y1 = apply_luma_tone(ctx, y0);

    if (ctx.use_spatial) {
        float y_ref = clamp(ctx.luma[idx], 0.0f, 1.0f);
        float mid_mask = smoothstep(0.10f, 0.60f, y_ref) * (1.0f - smoothstep(0.60f, 0.95f, y_ref));
        float detail_large = ctx.luma[idx] - ctx.blur_large[idx];
        float detail_small = ctx.luma[idx] - ctx.blur_small[idx];
        y1 += mid_mask * (ctx.clarity_amt * detail_large * 2.50f + ctx.texture_amt * detail_small * 1.75f);
        y1 = fmaxf(y1, 0.0f);
    }

    if (y0 > 1e-6f) {
        float s = y1 / y0;
        r *= s;
        g *= s;
        b *= s;
    } else {
        r = g = b = y1;
    }

    float luma = luminance709(r, g, b);
    float sat = ctx.saturation;
    r = luma + (r - luma) * sat;
    g = luma + (g - luma) * sat;
    b = luma + (b - luma) * sat;

    float chroma = fmaxf(r, fmaxf(g, b)) - fminf(r, fminf(g, b));
    float vib = ctx.vibrance;
    float yv, cbv, crv;
    rgb_to_ycbcr(r, g, b, &yv, &cbv, &crv);
    float skin_mask = smoothstep(0.02f, 0.22f, crv) * (1.0f - smoothstep(0.00f, 0.18f, cbv));
    float tone_pref = smoothstep(0.08f, 0.35f, luma) * (1.0f - smoothstep(0.75f, 0.98f, luma));
    float vib_boost = (1.0f - clamp(chroma, 0.0f, 1.0f)) * (0.55f + 0.45f * tone_pref) * (1.0f - 0.35f * skin_mask);
    float vib_gain = 1.0f + vib * vib_boost;
    r = luma + (r - luma) * vib_gain;
    g = luma + (g - luma) * vib_gain;
    b = luma + (b - luma) * vib_gain;

    if (ctx.use_nr) {
        float nr = ctx.nr_amt;

        float radius_f = 1.0f + 2.0f * nr;
        int radius = (int)ceilf(radius_f);
        float y_center = luminance709(r, g, b);
        float y_sum = 0.0f;
        float w_sum = 0.0f;

        for (int oy = -radius; oy <= radius; ++oy) {
            for (int ox = -radius; ox <= radius; ++ox) {
                float y_n, cb_n, cr_n;
                sample_linear_ycbcr(ctx, x + ox, y + oy, &y_n, &cb_n, &cr_n);
                float dsq = (float)(ox * ox + oy * oy);
                float spatial_w = expf(-dsq / (2.0f * radius_f * radius_f + 1e-6f));
                float range_w = expf(-fabsf(y_n - y_center) / (0.06f + 0.06f * (1.0f - nr)));
                float wgt = spatial_w * range_w;
                y_sum += y_n * wgt;
                w_sum += wgt;
            }
        }

        float denoised_luma = (w_sum > 1e-6f) ? (y_sum / w_sum) : y_center;
        float yl = luminance709(r, g, b);
        float target_luma = mixf(yl, denoised_luma, nr);
        if (yl > 1e-6f) {
            float ys = target_luma / yl;
            r *= ys;
            g *= ys;
            b *= ys;
        }
    }

    if (ctx.use_cnr) {
        float cnr = ctx.cnr_amt;
        float ycur, cbcur, crcur;
        rgb_to_ycbcr(r, g, b, &ycur, &cbcur, &crcur);

        float radius_f = 1.0f + cnr;
        int radius = (int)ceilf(radius_f);
        float cb_sum = 0.0f;
        float cr_sum = 0.0f;
        float w_sum = 0.0f;

        for (int oy = -radius; oy <= radius; ++oy) {
            for (int ox = -radius; ox <= radius; ++ox) {
                float y_n, cb_n, cr_n;
                sample_linear_ycbcr(ctx, x + ox, y + oy, &y_n, &cb_n, &cr_n);
                float dsq = (float)(ox * ox + oy * oy);
                float spatial_w = expf(-dsq / (2.0f * radius_f * radius_f + 1e-6f));
                float luma_w = expf(-fabsf(y_n - ycur) / 0.10f);
                float wgt = spatial_w * luma_w;
                cb_sum += cb_n * wgt;
                cr_sum += cr_n * wgt;
                w_sum += wgt;
            }
        }

        float cb_smooth = (w_sum > 1e-6f) ? (cb_sum / w_sum) : cbcur;
        float cr_smooth = (w_sum > 1e-6f) ? (cr_sum / w_sum) : crcur;
        cbcur = mixf(cbcur, cb_smooth, cnr);
        crcur = mixf(crcur, cr_smooth, cnr);
        ycbcr_to_rgb(ycur, cbcur, crcur, &r, &g, &b);
    }

    r = fmaxf(r, 0.0f);
    g = fmaxf(g, 0.0f);
    b = fmaxf(b, 0.0f);

    *out_r = srgb_encode_fast(r);
    *out_g = srgb_encode_fast(g);
    *out_b = srgb_encode_fast(b);
}

void process_image_pipeline() {
    if (!g_image.proxy_buffer || !g_image.full_buffer) return;
    auto process_begin = std::chrono::steady_clock::now();

    const uint64_t start_seq = g_request_seq.load(std::memory_order_relaxed);
    EditorParams params_snapshot = g_params;
    int lod_strength_snapshot = g_image.lod_strength;
    int histogram_quality_snapshot = g_image.histogram_quality;
    int histogram_interval_ms_snapshot = g_image.histogram_interval_ms;
    float zoom_snapshot = g_image.zoom;
    bool full_refresh_snapshot = g_full_refresh_requested.load(std::memory_order_relaxed);

    debug_log(
        "worker start seq=%llu zoom=%.3f lod=%d hist_q=%d hist_ms=%d full_refresh=%d exp=%.2f contrast=%.2f temp=%.0f tint=%.0f sat=%.2f vib=%.2f",
        (unsigned long long)start_seq,
        zoom_snapshot,
        lod_strength_snapshot,
        histogram_quality_snapshot,
        histogram_interval_ms_snapshot,
        full_refresh_snapshot ? 1 : 0,
        params_snapshot.exposure,
        params_snapshot.contrast,
        params_snapshot.temperature,
        params_snapshot.tint,
        params_snapshot.saturation,
        params_snapshot.vibrance);

    // Compute preview step before context setup so we can skip heavy filters
    // while zoomed out and keep interaction responsive.
    int base_step = lod_strength_snapshot;
    if (base_step < 1) base_step = 1;

    float zoom_clamped = zoom_snapshot;
    if (zoom_clamped < 0.1f) zoom_clamped = 0.1f;

    bool exposure_only_change = g_has_last_processed_params && params_equal_except_exposure(params_snapshot, g_last_processed_params);
    float exposure_ratio = powf(2.0f, params_snapshot.exposure - g_last_processed_params.exposure);
    if (exposure_only_change && fabsf(exposure_ratio - 1.0f) > 1e-4f) {
        debug_log("worker exposure-fast-path seq=%llu ratio=%.6f", (unsigned long long)start_seq, exposure_ratio);
        std::lock_guard<std::mutex> lock(g_buffer_mutex);
        int width = g_image.width;
        int height = g_image.height;
        if (g_image.working_buffer && width > 0 && height > 0) {
            unsigned int hw_threads = std::thread::hardware_concurrency();
            if (hw_threads == 0) hw_threads = 4;
            int threads = (int)std::min<unsigned int>(hw_threads, (unsigned int)std::max(height, 1));
            auto worker = [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    int row_idx = y * width * 3;
                    for (int x = 0; x < width; ++x) {
                        int idx = row_idx + x * 3;
                        float r = srgb_decode_fast(g_image.working_buffer[idx]);
                        float g = srgb_decode_fast(g_image.working_buffer[idx + 1]);
                        float b = srgb_decode_fast(g_image.working_buffer[idx + 2]);
                        r = fmaxf(r * exposure_ratio, 0.0f);
                        g = fmaxf(g * exposure_ratio, 0.0f);
                        b = fmaxf(b * exposure_ratio, 0.0f);
                        g_image.working_buffer[idx] = srgb_encode_fast(r);
                        g_image.working_buffer[idx + 1] = srgb_encode_fast(g);
                        g_image.working_buffer[idx + 2] = srgb_encode_fast(b);
                    }
                }
            };

            if (threads <= 1) {
                worker(0, height);
            } else {
                std::vector<std::thread> workers;
                workers.reserve((size_t)threads - 1);
                int chunk = (height + threads - 1) / threads;
                for (int t = 0; t < threads - 1; ++t) {
                    int y0 = t * chunk;
                    int y1 = std::min(height, y0 + chunk);
                    if (y0 < y1) workers.emplace_back(worker, y0, y1);
                }
                int my_y0 = (threads - 1) * chunk;
                if (my_y0 < height) worker(my_y0, height);
                for (auto& thread : workers) thread.join();
            }

            g_completed_x_start = 0;
            g_completed_y_start = 0;
            g_completed_x_end = width;
            g_completed_y_end = height;
            g_processing_complete = true;
            g_last_process_ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - process_begin).count();
            g_last_processed_params = params_snapshot;
            g_has_last_processed_params = true;
            debug_log("worker exposure-fast-path done seq=%llu process_ms=%.2f", (unsigned long long)start_seq, g_last_process_ms.load());
            return;
        }
    }

    bool turbo_enabled = g_turbo_preview_enabled.load(std::memory_order_relaxed);
    bool interaction_active = g_interaction_active.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(g_buffer_mutex);

    const float kProxyToFullZoom = 1.08f;
    bool use_proxy = g_image.using_proxy_source;
    if (use_proxy) {
        if (g_image.zoom > kProxyToFullZoom) use_proxy = false;
    } else {
        if (g_image.zoom <= 1.05f) use_proxy = true;
    }
    use_proxy = use_proxy && g_image.proxy_buffer && g_image.proxy_working_buffer;
    if (!use_proxy && (!g_image.full_buffer || !g_image.full_working_buffer)) {
        use_proxy = true;
    }

    if (use_proxy != g_image.using_proxy_source) {
        debug_log(
            "worker source switch seq=%llu %s -> %s zoom=%.3f",
            (unsigned long long)start_seq,
            g_image.using_proxy_source ? "proxy" : "full",
            use_proxy ? "proxy" : "full",
            g_image.zoom);
        g_image.using_proxy_source = use_proxy;
        if (use_proxy) {
            g_image.width = g_image.proxy_width;
            g_image.height = g_image.proxy_height;
            g_image.working_buffer = g_image.proxy_working_buffer;
        } else {
            g_image.width = g_image.full_width;
            g_image.height = g_image.full_height;
            g_image.working_buffer = g_image.full_working_buffer;
        }
        g_image.vis_x_start = 0;
        g_image.vis_y_start = 0;
        g_image.vis_x_end = g_image.width;
        g_image.vis_y_end = g_image.height;
    }

    g_active_source_buffer = use_proxy ? g_image.proxy_buffer : g_image.full_buffer;
    if (!g_active_source_buffer || !g_image.working_buffer) return;

    float source_scale_x = 1.0f;
    float source_scale_y = 1.0f;
    if (use_proxy && g_image.proxy_width > 0 && g_image.proxy_height > 0) {
        source_scale_x = (float)g_image.full_width / (float)g_image.proxy_width;
        source_scale_y = (float)g_image.full_height / (float)g_image.proxy_height;
    }
    float source_scale = fmaxf(source_scale_x, source_scale_y);

    float requested_total_decimation = (float)base_step / zoom_clamped;
    int step = (int)floorf((requested_total_decimation / source_scale) + 0.5f);
    if (step < 1) step = 1;

    int max_step_for_source = (int)floorf(((float)base_step / source_scale) + 0.5f);
    if (max_step_for_source < 1) max_step_for_source = 1;
    if (step > max_step_for_source) step = max_step_for_source;

    if (turbo_enabled && interaction_active && step < 2 && source_scale <= 1.25f) {
        step = 2;
    }

    bool fast_preview = (step > 1) || (turbo_enabled && interaction_active);

    debug_log(
        "worker config seq=%llu fast_preview=%d turbo=%d interact=%d step=%d source_scale=%.3f req_total_dec=%.3f",
        (unsigned long long)start_seq,
        fast_preview ? 1 : 0,
        turbo_enabled ? 1 : 0,
        interaction_active ? 1 : 0,
        step,
        source_scale,
        requested_total_decimation);

    ProcessingContext ctx = build_processing_context(fast_preview, params_snapshot);
    
    int start_x = g_image.vis_x_start;
    int start_y = g_image.vis_y_start;
    int end_x = g_image.vis_x_end;
    int end_y = g_image.vis_y_end;

    bool full_refresh = g_full_refresh_requested.exchange(false, std::memory_order_relaxed);
    if (full_refresh) {
        start_x = 0;
        start_y = 0;
        end_x = g_image.width;
        end_y = g_image.height;
        debug_log("worker full refresh seq=%llu region=0,0 -> %d,%d", (unsigned long long)start_seq, end_x, end_y);
    } else {
        debug_log("worker partial refresh seq=%llu region=%d,%d -> %d,%d", (unsigned long long)start_seq, start_x, start_y, end_x, end_y);
    }

    bool fit_view_active = zoom_snapshot <= 1.05f;
    if (fit_view_active && full_refresh && !fast_preview) {
        start_x = 0;
        start_y = 0;
        end_x = g_image.width;
        end_y = g_image.height;
    }

    // Align to step boundaries to prevent block shifting artifacts
    start_x = (start_x / step) * step;
    start_y = (start_y / step) * step;

    if (start_x < 0) start_x = 0;
    if (start_y < 0) start_y = 0;
    if (end_x > g_image.width) end_x = g_image.width;
    if (end_y > g_image.height) end_y = g_image.height;

    int span_w = end_x - start_x;
    int span_h = end_y - start_y;
    if (span_w < 0) span_w = 0;
    if (span_h < 0) span_h = 0;
    int sample_w = (span_w + step - 1) / step;
    int sample_h = (span_h + step - 1) / step;
    g_current_processing_step = step;
    g_current_effective_decimation = source_scale * (float)step;
    g_current_adaptive_pixels = sample_w * sample_h;

    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 4;

    std::atomic<bool> cancelled(false);

    auto render_rows = [&](int y_begin, int y_end) {
        for (int y = y_begin; y < y_end; ++y) {
            if (g_request_seq.load(std::memory_order_relaxed) != start_seq) {
                cancelled.store(true, std::memory_order_relaxed);
                return;
            }
            for (int x = start_x; x < end_x; ++x) {
                if (((x - start_x) & 15) == 0 && g_request_seq.load(std::memory_order_relaxed) != start_seq) {
                    cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                float out_r, out_g, out_b;
                process_pixel(ctx, x, y, &out_r, &out_g, &out_b);
                int fill_idx = (y * g_image.width + x) * 3;
                g_image.working_buffer[fill_idx] = out_r;
                g_image.working_buffer[fill_idx + 1] = out_g;
                g_image.working_buffer[fill_idx + 2] = out_b;
            }
        }
    };

    auto render_blocks = [&](int block_begin, int block_end) {
        for (int by = block_begin; by < block_end; ++by) {
            if (g_request_seq.load(std::memory_order_relaxed) != start_seq) {
                cancelled.store(true, std::memory_order_relaxed);
                return;
            }
            int y = start_y + by * step;
            for (int x = start_x; x < end_x; x += step) {
                if ((((x - start_x) / step) & 3) == 0 && g_request_seq.load(std::memory_order_relaxed) != start_seq) {
                    cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                int x1 = x + step;
                int y1 = y + step;
                if (x1 >= g_image.width) x1 = g_image.width - 1;
                if (y1 >= g_image.height) y1 = g_image.height - 1;

                float c00_r, c00_g, c00_b;
                float c10_r, c10_g, c10_b;
                float c01_r, c01_g, c01_b;
                float c11_r, c11_g, c11_b;
                process_pixel(ctx, x,  y,  &c00_r, &c00_g, &c00_b);
                process_pixel(ctx, x1, y,  &c10_r, &c10_g, &c10_b);
                process_pixel(ctx, x,  y1, &c01_r, &c01_g, &c01_b);
                process_pixel(ctx, x1, y1, &c11_r, &c11_g, &c11_b);

                int cell_h = step;
                int cell_w = step;
                if (y + cell_h > g_image.height) cell_h = g_image.height - y;
                if (x + cell_w > g_image.width) cell_w = g_image.width - x;

                for (int dy = 0; dy < cell_h; ++dy) {
                    if ((dy & 3) == 0 && g_request_seq.load(std::memory_order_relaxed) != start_seq) {
                        cancelled.store(true, std::memory_order_relaxed);
                        return;
                    }
                    float fy = (cell_h > 1) ? ((float)dy / (float)(cell_h - 1)) : 0.0f;
                    for (int dx = 0; dx < cell_w; ++dx) {
                        float fx = (cell_w > 1) ? ((float)dx / (float)(cell_w - 1)) : 0.0f;

                        float top_r = mixf(c00_r, c10_r, fx);
                        float top_g = mixf(c00_g, c10_g, fx);
                        float top_b = mixf(c00_b, c10_b, fx);
                        float bot_r = mixf(c01_r, c11_r, fx);
                        float bot_g = mixf(c01_g, c11_g, fx);
                        float bot_b = mixf(c01_b, c11_b, fx);

                        float out_r = mixf(top_r, bot_r, fy);
                        float out_g = mixf(top_g, bot_g, fy);
                        float out_b = mixf(top_b, bot_b, fy);

                        int fill_idx = ((y + dy) * g_image.width + (x + dx)) * 3;
                        g_image.working_buffer[fill_idx] = out_r;
                        g_image.working_buffer[fill_idx + 1] = out_g;
                        g_image.working_buffer[fill_idx + 2] = out_b;
                    }
                }
            }
        }
    };

    if (step == 1) {
        int total_rows = end_y - start_y;
        int threads = (int)std::min<unsigned int>(hw_threads, (unsigned int)std::max(total_rows, 1));
        debug_log("worker render rows seq=%llu rows=%d threads=%d", (unsigned long long)start_seq, total_rows, threads);
        if (threads <= 1) {
            render_rows(start_y, end_y);
        } else {
            std::vector<std::thread> workers;
            workers.reserve((size_t)threads - 1);
            int chunk = (total_rows + threads - 1) / threads;
            for (int t = 0; t < threads - 1; ++t) {
                int y0 = start_y + t * chunk;
                int y1 = y0 + chunk;
                if (y1 > end_y) y1 = end_y;
                if (y0 < y1) {
                    workers.emplace_back(render_rows, y0, y1);
                }
            }
            int my_y0 = start_y + (threads - 1) * chunk;
            if (my_y0 < end_y) {
                render_rows(my_y0, end_y);
            }
            for (size_t i = 0; i < workers.size(); ++i) {
                workers[i].join();
            }
        }
    } else {
        int total_blocks = (end_y - start_y + step - 1) / step;
        int threads = (int)std::min<unsigned int>(hw_threads, (unsigned int)std::max(total_blocks, 1));
        debug_log("worker render blocks seq=%llu blocks=%d threads=%d", (unsigned long long)start_seq, total_blocks, threads);
        if (threads <= 1) {
            render_blocks(0, total_blocks);
        } else {
            std::vector<std::thread> workers;
            workers.reserve((size_t)threads - 1);
            int chunk = (total_blocks + threads - 1) / threads;
            for (int t = 0; t < threads - 1; ++t) {
                int b0 = t * chunk;
                int b1 = b0 + chunk;
                if (b1 > total_blocks) b1 = total_blocks;
                if (b0 < b1) {
                    workers.emplace_back(render_blocks, b0, b1);
                }
            }
            int my_b0 = (threads - 1) * chunk;
            if (my_b0 < total_blocks) {
                render_blocks(my_b0, total_blocks);
            }
            for (size_t i = 0; i < workers.size(); ++i) {
                workers[i].join();
            }
        }
    }

    if (cancelled.load(std::memory_order_relaxed)) {
        debug_log("worker cancelled seq=%llu", (unsigned long long)start_seq);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    float hist_elapsed_ms = std::chrono::duration<float, std::milli>(now - g_last_histogram_compute).count();
    if (histogram_interval_ms_snapshot < 30) histogram_interval_ms_snapshot = 30;
    bool should_compute_histogram = (hist_elapsed_ms >= (float)histogram_interval_ms_snapshot);

    if (should_compute_histogram) {
        auto hist_begin = std::chrono::steady_clock::now();
        debug_log("worker histogram start seq=%llu step=%d", (unsigned long long)start_seq, histogram_quality_snapshot);
        // Histogram is computed from a low-res full-image proxy domain using user-selected decimation.
        int hist_step = histogram_quality_snapshot;
        if (hist_step < 1) hist_step = 1;

        int saved_width = g_image.width;
        int saved_height = g_image.height;
        int saved_vis_x0 = g_image.vis_x_start;
        int saved_vis_y0 = g_image.vis_y_start;
        int saved_vis_x1 = g_image.vis_x_end;
        int saved_vis_y1 = g_image.vis_y_end;
        const float* saved_active_src = g_active_source_buffer;

        g_image.width = g_image.proxy_width;
        g_image.height = g_image.proxy_height;
        g_image.vis_x_start = 0;
        g_image.vis_y_start = 0;
        g_image.vis_x_end = g_image.width;
        g_image.vis_y_end = g_image.height;
        g_active_source_buffer = g_image.proxy_buffer;
        // Histogram should reflect the final-grade pipeline, not fast preview shortcuts.
        ProcessingContext hist_ctx = build_processing_context(false, params_snapshot);

        g_image.width = saved_width;
        g_image.height = saved_height;
        g_image.vis_x_start = saved_vis_x0;
        g_image.vis_y_start = saved_vis_y0;
        g_image.vis_x_end = saved_vis_x1;
        g_image.vis_y_end = saved_vis_y1;
        g_active_source_buffer = saved_active_src;

        int hist_start_x = 0;
        int hist_start_y = 0;
        int hist_end_x = hist_ctx.width;
        int hist_end_y = hist_ctx.height;

        int hist_rows = (hist_end_y - hist_start_y + hist_step - 1) / hist_step;
        int hist_threads = (int)std::min<unsigned int>(hw_threads, (unsigned int)std::max(hist_rows, 1));
        std::vector<HistogramData> partial((size_t)hist_threads);
        for (int t = 0; t < hist_threads; ++t) {
            memset(&partial[(size_t)t], 0, sizeof(HistogramData));
        }

        auto hist_worker = [&](int t_idx, int row_begin, int row_end) {
            HistogramData& h = partial[(size_t)t_idx];
            for (int ry = row_begin; ry < row_end; ++ry) {
                if (g_request_seq.load(std::memory_order_relaxed) != start_seq) {
                    cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                int y = hist_start_y + ry * hist_step;
                for (int x = hist_start_x; x < hist_end_x; x += hist_step) {
                    if (g_request_seq.load(std::memory_order_relaxed) != start_seq) {
                        cancelled.store(true, std::memory_order_relaxed);
                        return;
                    }
                    float final_r, final_g, final_b;
                    process_pixel(hist_ctx, x, y, &final_r, &final_g, &final_b);

                    int bin_r = (int)(clamp(final_r, 0.0f, 1.0f) * 255.0f);
                    int bin_g = (int)(clamp(final_g, 0.0f, 1.0f) * 255.0f);
                    int bin_b = (int)(clamp(final_b, 0.0f, 1.0f) * 255.0f);

                    // Convert to linear light for physically meaningful luminance,
                    // then map back to display-referred code value for histogram binning.
                    float lin_r = srgb_decode_fast(final_r);
                    float lin_g = srgb_decode_fast(final_g);
                    float lin_b = srgb_decode_fast(final_b);
                    float luma_linear = luminance709(lin_r, lin_g, lin_b);
                    int bin_luma = (int)(clamp(srgb_encode_fast(luma_linear), 0.0f, 1.0f) * 255.0f);

                    int wx = hist_step;
                    int wy = hist_step;
                    if (x + wx > hist_end_x) wx = hist_end_x - x;
                    if (y + wy > hist_end_y) wy = hist_end_y - y;
                    float sample_weight = (float)(wx * wy);

                    h.r[bin_r] += sample_weight;
                    h.g[bin_g] += sample_weight;
                    h.b[bin_b] += sample_weight;
                    h.luma[bin_luma] += sample_weight;
                }
            }
        };

        if (hist_threads <= 1) {
            hist_worker(0, 0, hist_rows);
        } else {
            std::vector<std::thread> workers;
            workers.reserve((size_t)hist_threads - 1);
            int chunk = (hist_rows + hist_threads - 1) / hist_threads;
            for (int t = 0; t < hist_threads - 1; ++t) {
                int r0 = t * chunk;
                int r1 = r0 + chunk;
                if (r1 > hist_rows) r1 = hist_rows;
                if (r0 < r1) {
                    workers.emplace_back(hist_worker, t, r0, r1);
                }
            }
            int my_r0 = (hist_threads - 1) * chunk;
            if (my_r0 < hist_rows) {
                hist_worker(hist_threads - 1, my_r0, hist_rows);
            }
            for (size_t i = 0; i < workers.size(); ++i) {
                workers[i].join();
            }
        }

        if (cancelled.load(std::memory_order_relaxed)) {
            debug_log("worker histogram cancelled seq=%llu", (unsigned long long)start_seq);
            return;
        }

        memset(&g_image.histogram, 0, sizeof(HistogramData));
        for (int t = 0; t < hist_threads; ++t) {
            for (int i = 0; i < 256; ++i) {
                g_image.histogram.r[i] += partial[(size_t)t].r[i];
                g_image.histogram.g[i] += partial[(size_t)t].g[i];
                g_image.histogram.b[i] += partial[(size_t)t].b[i];
                g_image.histogram.luma[i] += partial[(size_t)t].luma[i];
            }
        }

        g_image.histogram.max_val = 0.0f;
        for (int i = 0; i < 256; i++) {
            if (g_image.histogram.r[i] > g_image.histogram.max_val) g_image.histogram.max_val = g_image.histogram.r[i];
            if (g_image.histogram.g[i] > g_image.histogram.max_val) g_image.histogram.max_val = g_image.histogram.g[i];
            if (g_image.histogram.b[i] > g_image.histogram.max_val) g_image.histogram.max_val = g_image.histogram.b[i];
            if (g_image.histogram.luma[i] > g_image.histogram.max_val) g_image.histogram.max_val = g_image.histogram.luma[i];
        }

        g_last_histogram_compute = now;
        auto hist_end = std::chrono::steady_clock::now();
        g_last_histogram_ms = std::chrono::duration<float, std::milli>(hist_end - hist_begin).count();
        debug_log("worker histogram done seq=%llu hist_ms=%.2f max=%.1f", (unsigned long long)start_seq, g_last_histogram_ms.load(), g_image.histogram.max_val);
    } else {
        debug_log(
            "worker histogram skipped seq=%llu elapsed=%.2f threshold=%d turbo=%d interact=%d",
            (unsigned long long)start_seq,
            hist_elapsed_ms,
            histogram_interval_ms_snapshot,
            turbo_enabled ? 1 : 0,
            interaction_active ? 1 : 0);
    }

    auto process_end = std::chrono::steady_clock::now();
    g_last_process_ms = std::chrono::duration<float, std::milli>(process_end - process_begin).count();
    g_completed_x_start = start_x;
    g_completed_y_start = start_y;
    g_completed_x_end = end_x;
    g_completed_y_end = end_y;
    g_last_processed_params = params_snapshot;
    g_has_last_processed_params = true;
    g_processing_complete = true;
    debug_log("worker done seq=%llu process_ms=%.2f region=%d,%d -> %d,%d", (unsigned long long)start_seq, g_last_process_ms.load(), start_x, start_y, end_x, end_y);
}

void processing_worker() {
    uint64_t last_seq = g_request_seq.load();
    while (g_keep_running) {
        {
            std::unique_lock<std::mutex> lock(g_request_mutex);
            g_request_cv.wait(lock, [&]() {
                return !g_keep_running || g_request_seq.load() != last_seq;
            });
            if (!g_keep_running) {
                break;
            }
            last_seq = g_request_seq.load();
        }
        process_image_pipeline();
    }
}

void request_processing() {
    debug_log(
        "request_processing seq=%llu full_refresh=%d zoom=%.3f lod=%d vis=%d,%d -> %d,%d",
        (unsigned long long)g_request_seq.load(std::memory_order_relaxed),
        g_full_refresh_requested.load(std::memory_order_relaxed) ? 1 : 0,
        g_image.zoom,
        g_image.lod_strength,
        g_image.vis_x_start,
        g_image.vis_y_start,
        g_image.vis_x_end,
        g_image.vis_y_end);
    g_request_seq.fetch_add(1, std::memory_order_relaxed);
    g_request_cv.notify_one();
}

// -----------------------------------------------------------------------------
// LibRaw Integration
// -----------------------------------------------------------------------------
void load_raw(const char* filepath, bool request_processing_after_load) {
    if (filepath && filepath[0]) {
        snprintf(g_current_image_path, sizeof(g_current_image_path), "%s", filepath);
    } else {
        g_current_image_path[0] = '\0';
    }

    debug_log("load_raw begin path=%s", g_current_image_path[0] ? g_current_image_path : "(empty)");
    g_load_progress = 0.0f;

    std::lock_guard<std::mutex> export_lock(g_export_mutex);
    
    libraw_data_t *RawProcessor = libraw_init(0);
    if (!RawProcessor) {
        g_load_success = false;
        return;
    }

    libraw_set_progress_handler(RawProcessor, load_progress_callback, NULL);
    
    RawProcessor->params.use_camera_wb = 1;
    RawProcessor->params.half_size = 0;
    RawProcessor->params.no_auto_bright = 1;
    RawProcessor->params.output_bps = 16;
    RawProcessor->params.output_color = 1;
    RawProcessor->params.gamm[0] = 1.0f;
    RawProcessor->params.gamm[1] = 1.0f;
    
    if (libraw_open_file(RawProcessor, filepath) != LIBRAW_SUCCESS) {
        printf("Failed to open RAW file.\n");
        debug_log("load_raw open failed path=%s", filepath ? filepath : "(null)");
        g_load_success = false;
        libraw_close(RawProcessor);
        return;
    }
    
    if (libraw_unpack(RawProcessor) != LIBRAW_SUCCESS) {
        debug_log("load_raw unpack failed path=%s", filepath ? filepath : "(null)");
        g_load_success = false;
        libraw_close(RawProcessor);
        return;
    }

    float r_mul = libraw_get_cam_mul(RawProcessor, 0);
    float b_mul = libraw_get_cam_mul(RawProcessor, 2);
    if (r_mul > 0.0f && b_mul > 0.0f) {
        float ratio = b_mul / r_mul;
        float estimated_temp = 5000.0f * ratio;
        if (estimated_temp < kMinTempK) estimated_temp = kMinTempK;
        if (estimated_temp > kMaxTempK) estimated_temp = kMaxTempK;
        g_image.image_temperature = estimated_temp;
        g_params.temperature = estimated_temp;
    } else {
        g_image.image_temperature = g_default_params.temperature;
    }
    
    snprintf(g_image.metadata.camera_maker, sizeof(g_image.metadata.camera_maker), "%s", RawProcessor->idata.make);
    snprintf(g_image.metadata.camera_model, sizeof(g_image.metadata.camera_model), "%s", RawProcessor->idata.model);
    snprintf(g_image.metadata.lens_name, sizeof(g_image.metadata.lens_name), "%s", RawProcessor->lens.Lens);
    g_image.metadata.iso_speed = RawProcessor->other.iso_speed;
    g_image.metadata.shutter = RawProcessor->other.shutter;
    g_image.metadata.aperture = RawProcessor->other.aperture;
    g_image.metadata.focal_length = RawProcessor->other.focal_len;
    snprintf(g_image.metadata.timestamp, sizeof(g_image.metadata.timestamp), "%s", asctime(gmtime(&RawProcessor->other.timestamp)));
    snprintf(g_image.metadata.artist, sizeof(g_image.metadata.artist), "%s", RawProcessor->other.artist);
    
    if (libraw_dcraw_process(RawProcessor) != LIBRAW_SUCCESS) {
        debug_log("load_raw dcraw_process failed path=%s", filepath ? filepath : "(null)");
        g_load_success = false;
        libraw_close(RawProcessor);
        return;
    }

    int err = 0;
    libraw_processed_image_t* image = libraw_dcraw_make_mem_image(RawProcessor, &err);
    if (!image || image->type != LIBRAW_IMAGE_BITMAP || image->colors != 3) {
        printf("Unsupported image format decoded.\n");
        debug_log("load_raw unsupported decoded image path=%s", filepath ? filepath : "(null)");
        if (image) libraw_dcraw_clear_mem(image);
        g_load_success = false;
        libraw_close(RawProcessor);
        return;
    }

    // Ensure buffer replacement is serialized against in-flight processing.
    std::lock_guard<std::mutex> lock(g_buffer_mutex);
    g_has_last_processed_params = false;
    g_full_refresh_requested = true;

    if (g_image.proxy_buffer) free(g_image.proxy_buffer);
    if (g_image.full_buffer) free(g_image.full_buffer);
    if (g_image.proxy_working_buffer) free(g_image.proxy_working_buffer);
    if (g_image.full_working_buffer) free(g_image.full_working_buffer);

    g_image.full_width = image->width;
    g_image.full_height = image->height;

    int full_count = g_image.full_width * g_image.full_height * 3;
    size_t full_buffer_size = (size_t)full_count * sizeof(float);
    g_image.full_buffer = (float*)malloc(full_buffer_size);
    g_image.full_working_buffer = (float*)malloc(full_buffer_size);
    if (!g_image.full_buffer || !g_image.full_working_buffer) {
        if (g_image.full_buffer) free(g_image.full_buffer);
        if (g_image.full_working_buffer) free(g_image.full_working_buffer);
        g_image.full_buffer = NULL;
        g_image.full_working_buffer = NULL;
        g_load_success = false;
        libraw_dcraw_clear_mem(image);
        libraw_close(RawProcessor);
        return;
    }

    if (image->bits == 16) {
        const uint16_t* src16 = (const uint16_t*)image->data;
        for (int i = 0; i < full_count; i++) {
            g_image.full_buffer[i] = (float)src16[i] / 65535.0f;
        }
    } else {
        for (int i = 0; i < full_count; i++) {
            g_image.full_buffer[i] = (float)image->data[i] / 255.0f;
        }
    }

    const int proxy_max_edge = 2048;
    int long_edge = std::max(g_image.full_width, g_image.full_height);
    float proxy_scale = (long_edge > proxy_max_edge) ? ((float)proxy_max_edge / (float)long_edge) : 1.0f;
    g_image.proxy_width = std::max(1, (int)floorf(g_image.full_width * proxy_scale));
    g_image.proxy_height = std::max(1, (int)floorf(g_image.full_height * proxy_scale));

    size_t proxy_count = (size_t)g_image.proxy_width * (size_t)g_image.proxy_height * 3;
    g_image.proxy_buffer = (float*)malloc(proxy_count * sizeof(float));
    g_image.proxy_working_buffer = (float*)malloc(proxy_count * sizeof(float));
    if (!g_image.proxy_buffer || !g_image.proxy_working_buffer) {
        if (g_image.proxy_buffer) free(g_image.proxy_buffer);
        if (g_image.proxy_working_buffer) free(g_image.proxy_working_buffer);
        if (g_image.full_buffer) free(g_image.full_buffer);
        if (g_image.full_working_buffer) free(g_image.full_working_buffer);
        g_image.proxy_buffer = NULL;
        g_image.proxy_working_buffer = NULL;
        g_image.full_buffer = NULL;
        g_image.full_working_buffer = NULL;
        g_load_success = false;
        libraw_dcraw_clear_mem(image);
        libraw_close(RawProcessor);
        return;
    }

    for (int py = 0; py < g_image.proxy_height; ++py) {
        float src_y = ((float)py + 0.5f) * ((float)g_image.full_height / (float)g_image.proxy_height) - 0.5f;
        int y0 = (int)floorf(src_y);
        int y1 = y0 + 1;
        float fy = src_y - (float)y0;
        if (y0 < 0) y0 = 0;
        if (y1 >= g_image.full_height) y1 = g_image.full_height - 1;

        for (int px = 0; px < g_image.proxy_width; ++px) {
            float src_x = ((float)px + 0.5f) * ((float)g_image.full_width / (float)g_image.proxy_width) - 0.5f;
            int x0 = (int)floorf(src_x);
            int x1 = x0 + 1;
            float fx = src_x - (float)x0;
            if (x0 < 0) x0 = 0;
            if (x1 >= g_image.full_width) x1 = g_image.full_width - 1;

            int pidx = (py * g_image.proxy_width + px) * 3;
            int i00 = (y0 * g_image.full_width + x0) * 3;
            int i10 = (y0 * g_image.full_width + x1) * 3;
            int i01 = (y1 * g_image.full_width + x0) * 3;
            int i11 = (y1 * g_image.full_width + x1) * 3;

            for (int c = 0; c < 3; ++c) {
                float top = mixf(g_image.full_buffer[i00 + c], g_image.full_buffer[i10 + c], fx);
                float bot = mixf(g_image.full_buffer[i01 + c], g_image.full_buffer[i11 + c], fx);
                g_image.proxy_buffer[pidx + c] = mixf(top, bot, fy);
            }
        }
    }

    g_image.using_proxy_source = true;
    g_image.width = g_image.proxy_width;
    g_image.height = g_image.proxy_height;
    g_image.working_buffer = g_image.proxy_working_buffer;

    libraw_dcraw_clear_mem(image);
    libraw_close(RawProcessor);

    g_image.zoom = 1.0f;
    g_image.last_zoom = 1.0f;
    g_image.pan_x = 0.0f;
    g_image.pan_y = 0.0f;
    g_image.vis_x_start = 0;
    g_image.vis_y_start = 0;
    g_image.vis_x_end = g_image.width;
    g_image.vis_y_end = g_image.height;

    debug_log(
        "load_raw ready full=%dx%d proxy=%dx%d zoom=%.2f",
        g_image.full_width,
        g_image.full_height,
        g_image.proxy_width,
        g_image.proxy_height,
        g_image.zoom);
    
    g_load_progress = 1.0f;
    g_load_success = true;
    if (request_processing_after_load) {
        request_processing();
    }
}

void upload_loaded_image_textures() {
    if (!g_image.proxy_buffer || g_image.width <= 0 || g_image.height <= 0) return;

    int count = g_image.width * g_image.height * 3;

    if (g_image.texture_id == 0) glGenTextures(1, &g_image.texture_id);
    if (g_image.original_texture_id == 0) glGenTextures(1, &g_image.original_texture_id);

    g_image.texture_width = g_image.width;
    g_image.texture_height = g_image.height;
    g_image.original_texture_width = g_image.width;
    g_image.original_texture_height = g_image.height;

    glBindTexture(GL_TEXTURE_2D, g_image.texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, g_image.width, g_image.height, 0, GL_RGB, GL_FLOAT, g_image.proxy_buffer);

    std::vector<float> original_display_buffer((size_t)count);
    for (int i = 0; i < count; ++i) {
        original_display_buffer[(size_t)i] = srgb_encode_fast(g_image.proxy_buffer[i]);
    }

    glBindTexture(GL_TEXTURE_2D, g_image.original_texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, g_image.width, g_image.height, 0, GL_RGB, GL_FLOAT, original_display_buffer.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void request_load_raw_async(const char* filepath) {
    if (!filepath || !filepath[0]) return;

    debug_log("request_load_raw_async path=%s", filepath);

    bool expected = false;
    if (!g_load_in_progress.compare_exchange_strong(expected, true)) {
        return;
    }

    g_load_done = false;
    g_load_success = false;
    g_load_progress = 0.0f;

    std::string load_path(filepath);
    std::thread load_thread([load_path]() {
        load_raw(load_path.c_str(), false);
        g_load_done = true;
        g_load_in_progress = false;
    });
    load_thread.detach();
}

void request_thumbnail_preview(const char* filepath) {
    if (!filepath || !filepath[0]) return;

    std::lock_guard<std::mutex> lock(g_thumbnail_mutex);
    std::string path(filepath);
    if (g_thumbnail_seen.find(path) != g_thumbnail_seen.end()) return;
    g_thumbnail_seen.insert(path);
    g_thumbnail_queue.push_back(path);
    g_thumbnail_cv.notify_one();
}

bool pop_thumbnail_preview(ThumbnailPreview* out_preview) {
    if (!out_preview) return false;

    std::lock_guard<std::mutex> lock(g_thumbnail_mutex);
    if (g_thumbnail_ready_queue.empty()) return false;
    *out_preview = std::move(g_thumbnail_ready_queue.front());
    g_thumbnail_ready_queue.pop_front();
    return true;
}

void thumbnail_worker() {
    while (g_keep_running) {
        std::string path;
        {
            std::unique_lock<std::mutex> lock(g_thumbnail_mutex);
            g_thumbnail_cv.wait(lock, [] {
                return !g_keep_running || !g_thumbnail_queue.empty();
            });
            if (!g_keep_running) break;
            path = std::move(g_thumbnail_queue.front());
            g_thumbnail_queue.pop_front();
        }

        ThumbnailPreview preview = {};
        if (decode_raw_thumbnail(path.c_str(), &preview)) {
            std::lock_guard<std::mutex> lock(g_thumbnail_mutex);
            g_thumbnail_ready_queue.push_back(std::move(preview));
        }
    }
}

void notify_thumbnail_worker_shutdown() {
    g_thumbnail_cv.notify_all();
}

bool export_image(const char* filepath) {
    if (!g_image.full_buffer || !filepath || !filepath[0]) return false;

    debug_log("export begin path=%s", filepath);

    std::lock_guard<std::mutex> export_lock(g_export_mutex);
    std::lock_guard<std::mutex> snapshot_lock(g_buffer_mutex);
    g_export_progress = 0.0f;

    EditorParams params_snapshot = g_params;
    int width = g_image.full_width;
    int height = g_image.full_height;
    const float* source_buffer_snapshot = g_image.full_buffer;
    float source_temperature_snapshot = g_image.image_temperature;
    ProcessingContext ctx = build_processing_context_from_snapshot(
        false,
        params_snapshot,
        width,
        height,
        0,
        0,
        width,
        height,
        source_buffer_snapshot,
        source_temperature_snapshot);
    
    uint8_t* out_pixels = (uint8_t*)malloc(width * height * 3);
    if (!out_pixels) return false;

    bool ok = false;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;
            
            float final_r, final_g, final_b;
            process_pixel(ctx, x, y, &final_r, &final_g, &final_b);

            out_pixels[idx]     = (uint8_t)(clamp(final_r, 0.0f, 1.0f) * 255.0f);
            out_pixels[idx + 1] = (uint8_t)(clamp(final_g, 0.0f, 1.0f) * 255.0f);
            out_pixels[idx + 2] = (uint8_t)(clamp(final_b, 0.0f, 1.0f) * 255.0f);
        }
        g_export_progress = (float)(y + 1) / (float)height;
        if ((y % 256) == 0 || y + 1 == height) {
            debug_log("export progress path=%s row=%d/%d pct=%.1f", filepath, y + 1, height, g_export_progress.load() * 100.0f);
        }
    }

    const char* ext = strrchr(filepath, '.');
    if (ext) {
        bool is_jpg = false;
        const char* jpg_exts[] = {".jpg", ".jpeg", ".JPG", ".JPEG"};
        for (int i = 0; i < 4; i++) {
            if (strcmp(ext, jpg_exts[i]) == 0) is_jpg = true;
        }

        if (is_jpg) {
            ok = stbi_write_jpg(filepath, width, height, 3, out_pixels, 95) != 0;
        } else {
            ok = stbi_write_png(filepath, width, height, 3, out_pixels, width * 3) != 0;
        }
    }
    
    free(out_pixels);
    g_export_progress = 1.0f;
    debug_log("export done path=%s ok=%d", filepath, ok ? 1 : 0);
    return ok;
}

void request_export_async(const char* filepath) {
    if (!filepath || !filepath[0]) return;
    if (!g_image.full_buffer) return;

    debug_log("request_export_async path=%s", filepath);

    bool expected = false;
    if (!g_export_in_progress.compare_exchange_strong(expected, true)) {
        return;
    }

    g_export_done = false;
    g_export_success = false;
    g_export_progress = 0.0f;

    std::string export_path(filepath);
    std::thread export_thread([export_path]() {
        bool ok = export_image(export_path.c_str());

        g_export_success = ok;
        g_export_done = true;
        g_export_in_progress = false;
    });
    export_thread.detach();
}