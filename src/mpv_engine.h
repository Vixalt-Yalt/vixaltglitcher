#ifndef VIX_MPV_ENGINE_H
#define VIX_MPV_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#define VIX_MAX_PLAYLIST 512
#define VIX_PATH_MAX     2048
#define VIX_TITLE_MAX    256

typedef enum VixGlitchPreset {
    VIX_FX_OFF = 0,
    VIX_FX_RGB_GLITCH,
    VIX_FX_NEON_LILAC,
    VIX_FX_CRT_SCAN,
    VIX_FX_CYBER_BOOST,
    VIX_FX_COUNT
} VixGlitchPreset;

typedef struct VixPlaylistItem {
    char path[VIX_PATH_MAX];
    char title[VIX_TITLE_MAX];
} VixPlaylistItem;

typedef struct VixMpvEngine {
    void *dl_handle;
    void *mpv;        /* mpv_handle* */
    void *mpv_gl;     /* mpv_render_context* */

    bool libmpv_available;
    bool gl_ready;
    char libmpv_soname[256];
    char status_error[256];
    unsigned long api_version;

    uint32_t sdl_wakeup_event;
    uint32_t sdl_render_event;
    bool needs_render;

    /* Observed playback state */
    bool idle_active;
    bool paused;
    bool eof_reached;
    bool muted;
    bool sub_visible;

    double time_pos;
    double duration;
    double cache_time;
    double volume;
    double speed;
    double fps;
    double ab_loop_a;
    double ab_loop_b;

    int64_t video_w;
    int64_t video_h;
    int64_t playlist_pos;
    int64_t playlist_count;

    char media_title[VIX_TITLE_MAX];
    char current_path[VIX_PATH_MAX];
    char video_codec[64];
    char audio_codec[64];
    char hwdec_current[64];
    char aid_str[32];
    char sid_str[32];
    char screenshot_dir[VIX_PATH_MAX];

    VixGlitchPreset active_fx;

    /* Synchronized playlist metadata for the UI drawer */
    VixPlaylistItem items[VIX_MAX_PLAYLIST];
    int item_count;
} VixMpvEngine;

bool mpv_engine_init(VixMpvEngine *eng, const char *app_base_dir,
                     uint32_t wakeup_event, uint32_t render_event);
bool mpv_engine_init_gl(VixMpvEngine *eng, void *(*get_proc_address)(void *ctx, const char *name), void *proc_ctx);
void mpv_engine_destroy(VixMpvEngine *eng);

/* Event & Render loop integration */
void mpv_engine_process_events(VixMpvEngine *eng);
bool mpv_engine_check_render_update(VixMpvEngine *eng);
void mpv_engine_render_gl(VixMpvEngine *eng, int fbo, int width, int height);
void mpv_engine_report_swap(VixMpvEngine *eng);

/* Playback & Playlist commands */
bool mpv_engine_load(VixMpvEngine *eng, const char *path_or_url, bool append_to_playlist);
void mpv_engine_play_index(VixMpvEngine *eng, int index);
void mpv_engine_remove_index(VixMpvEngine *eng, int index);
void mpv_engine_clear_playlist(VixMpvEngine *eng);
void mpv_engine_prev(VixMpvEngine *eng);
void mpv_engine_next(VixMpvEngine *eng);

void mpv_engine_toggle_pause(VixMpvEngine *eng);
void mpv_engine_set_pause(VixMpvEngine *eng, bool pause);
void mpv_engine_stop(VixMpvEngine *eng);
void mpv_engine_seek_relative(VixMpvEngine *eng, double seconds);
void mpv_engine_seek_absolute(VixMpvEngine *eng, double seconds);
void mpv_engine_frame_step(VixMpvEngine *eng, bool backward);

void mpv_engine_set_volume(VixMpvEngine *eng, double vol);
void mpv_engine_add_volume(VixMpvEngine *eng, double delta);
void mpv_engine_toggle_mute(VixMpvEngine *eng);
void mpv_engine_set_speed(VixMpvEngine *eng, double speed);
void mpv_engine_cycle_speed(VixMpvEngine *eng, int direction);

void mpv_engine_cycle_audio(VixMpvEngine *eng);
void mpv_engine_cycle_sub(VixMpvEngine *eng);
void mpv_engine_toggle_sub_visibility(VixMpvEngine *eng);
bool mpv_engine_load_subtitle(VixMpvEngine *eng, const char *sub_path);

const char *mpv_engine_fx_name(VixGlitchPreset preset);
void mpv_engine_set_fx(VixMpvEngine *eng, VixGlitchPreset preset);
void mpv_engine_cycle_fx(VixMpvEngine *eng);

bool mpv_engine_screenshot(VixMpvEngine *eng, char *out_msg, int out_msg_len);
void mpv_engine_ab_loop(VixMpvEngine *eng, char *out_msg, int out_msg_len);
void mpv_engine_adjust_eq(VixMpvEngine *eng, const char *prop, int delta);

#endif /* VIX_MPV_ENGINE_H */
