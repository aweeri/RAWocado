#pragma once

#include <stdint.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <cstdarg>
#include <vector>
#include <GLFW/glfw3.h> // For GLuint

struct ImageMetadata {
    char camera_maker[64];
    char camera_model[64];
    char lens_name[128];
    float iso_speed;
    float shutter;
    float aperture;
    float focal_length;
    char timestamp[32];    
    char artist[64];        
};

struct HistogramData {
    float r[256];
    float g[256];
    float b[256];
    float luma[256];
    float max_val;
    float black_clip_count;
    float white_clip_count;
    float sample_count;
};

struct ImageState {
    int width;
    int height;
    int full_width;
    int full_height;
    int proxy_width;
    int proxy_height;
    GLuint texture_id;
    GLuint original_texture_id;
    int texture_width;
    int texture_height;
    int original_texture_width;
    int original_texture_height;
    float* proxy_buffer;
    float* full_buffer;
    float* working_buffer;
    float* proxy_working_buffer;
    float* full_working_buffer;
    bool using_proxy_source;
    float zoom;
    float pan_x;
    float pan_y;
    ImageMetadata metadata;
    HistogramData histogram;
    float image_temperature;
    bool show_r;
    bool show_g;
    bool show_b;
    bool show_luma;
    int lod_strength;         
    int histogram_quality;    
    int histogram_interval_ms;
    float last_zoom;          
    int vis_x_start;
    int vis_y_start;
    int vis_x_end;
    int vis_y_end;
};

struct EditorParams {
    float exposure;
    float contrast;
    float highlights;
    float midtones;
    float shadows;
    float whites;
    float blacks;
    
    float temperature;
    float tint;
    float vibrance;
    float saturation;
    
    float texture;
    float clarity;
    float dehaze;
    
    float noise_reduction;
    float color_noise_reduction;
};

struct ThumbnailPreview {
    char path[1024];
    int width;
    int height;
    std::vector<uint8_t> rgba_pixels;
};

extern ImageState g_image;
extern EditorParams g_params;
extern const EditorParams g_default_params;

extern std::atomic<bool> g_processing_complete;
extern std::atomic<bool> g_keep_running;
extern std::atomic<bool> g_interaction_active;
extern std::atomic<bool> g_turbo_preview_enabled;
extern std::atomic<bool> g_export_in_progress;
extern std::atomic<bool> g_export_done;
extern std::atomic<bool> g_export_success;
extern std::atomic<float> g_export_progress;
extern std::atomic<bool> g_load_in_progress;
extern std::atomic<bool> g_load_done;
extern std::atomic<bool> g_load_success;
extern std::atomic<float> g_load_progress;
extern std::atomic<float> g_last_process_ms;
extern std::atomic<float> g_last_histogram_ms;
extern std::atomic<int> g_current_processing_step;
extern std::atomic<float> g_current_effective_decimation;
extern std::atomic<int> g_current_adaptive_pixels;
extern std::atomic<bool> g_full_refresh_requested;
extern std::atomic<int> g_completed_x_start;
extern std::atomic<int> g_completed_y_start;
extern std::atomic<int> g_completed_x_end;
extern std::atomic<int> g_completed_y_end;
extern std::mutex g_buffer_mutex;
extern char g_current_image_path[1024];
extern std::mutex g_request_mutex;
extern std::condition_variable g_request_cv;
extern std::atomic<uint64_t> g_request_seq;

void process_image_pipeline();
void processing_worker();
void request_processing();
void load_raw(const char* filepath, bool request_processing_after_load = true);
void request_load_raw_async(const char* filepath);
void upload_loaded_image_textures();
void request_thumbnail_preview(const char* filepath);
bool pop_thumbnail_preview(ThumbnailPreview* out_preview);
void thumbnail_worker();
void notify_thumbnail_worker_shutdown();
bool export_image(const char* filepath);
void request_export_async(const char* filepath);
void debug_log(const char* fmt, ...);