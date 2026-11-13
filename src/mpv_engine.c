#define _GNU_SOURCE
#include "mpv_engine.h"
#include "mpv/client.h"
#include "mpv/render.h"
#include "mpv/render_gl.h"

#include <SDL.h>
#include <dlfcn.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct MpvSymTable {
    unsigned long (*client_api_version)(void);
    mpv_handle *(*create)(void);
    int (*initialize)(mpv_handle *ctx);
    void (*terminate_destroy)(mpv_handle *ctx);
    const char *(*error_string)(int error);
    void (*free)(void *data);

    int (*set_option)(mpv_handle *ctx, const char *name, mpv_format format, void *data);
    int (*set_option_string)(mpv_handle *ctx, const char *name, const char *data);
    int (*load_config_file)(mpv_handle *ctx, const char *filename);

    int (*command)(mpv_handle *ctx, const char **args);
    int (*command_async)(mpv_handle *ctx, uint64_t reply_userdata, const char **args);
    int (*command_string)(mpv_handle *ctx, const char *args);

    int (*set_property)(mpv_handle *ctx, const char *name, mpv_format format, void *data);
    int (*set_property_string)(mpv_handle *ctx, const char *name, const char *data);
    int (*set_property_async)(mpv_handle *ctx, uint64_t reply_userdata, const char *name, mpv_format format, void *data);
    int (*get_property)(mpv_handle *ctx, const char *name, mpv_format format, void *data);
    char *(*get_property_string)(mpv_handle *ctx, const char *name);
    int (*observe_property)(mpv_handle *mpv, uint64_t reply_userdata, const char *name, mpv_format format);

    void (*set_wakeup_callback)(mpv_handle *ctx, void (*cb)(void *d), void *d);
    mpv_event *(*wait_event)(mpv_handle *ctx, double timeout);

    int (*render_context_create)(mpv_render_context **res, mpv_handle *mpv, mpv_render_param *params);
    void (*render_context_set_update_callback)(mpv_render_context *ctx, mpv_render_update_fn callback, void *callback_ctx);
    uint64_t (*render_context_update)(mpv_render_context *ctx);
    int (*render_context_render)(mpv_render_context *ctx, mpv_render_param *params);
    void (*render_context_report_swap)(mpv_render_context *ctx);
    void (*render_context_free)(mpv_render_context *ctx);
} MpvSymTable;

static MpvSymTable fn;

static void extract_display_title(const char *path_or_url, char *out, size_t out_sz) {
    if (!path_or_url || !*path_or_url) {
        snprintf(out, out_sz, "Untitled");
        return;
    }
    if (strstr(path_or_url, "://")) {
        snprintf(out, out_sz, "%s", path_or_url);
        return;
    }
    const char *slash = strrchr(path_or_url, '/');
    const char *base = slash ? (slash + 1) : path_or_url;
    if (!*base) base = path_or_url;
    snprintf(out, out_sz, "%s", base);
}

static void mkdir_p(const char *dir) {
    char tmp[VIX_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void on_mpv_wakeup(void *ctx) {
    VixMpvEngine *eng = (VixMpvEngine *)ctx;
    if (eng->sdl_wakeup_event != 0) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = eng->sdl_wakeup_event;
        SDL_PushEvent(&ev);
    }
}

static void on_mpv_render_update(void *ctx) {
    VixMpvEngine *eng = (VixMpvEngine *)ctx;
    if (eng->sdl_render_event != 0) {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = eng->sdl_render_event;
        SDL_PushEvent(&ev);
    }
}

#define RESOLVE_SYM(field, sym_name) do { \
    *(void **)(&fn.field) = dlsym(handle, sym_name); \
    if (!fn.field) return false; \
} while (0)

static bool load_mpv_symbols(void *handle) {
    memset(&fn, 0, sizeof(fn));
    RESOLVE_SYM(client_api_version, "mpv_client_api_version");
    RESOLVE_SYM(create, "mpv_create");
    RESOLVE_SYM(initialize, "mpv_initialize");
    RESOLVE_SYM(terminate_destroy, "mpv_terminate_destroy");
    RESOLVE_SYM(error_string, "mpv_error_string");
    RESOLVE_SYM(free, "mpv_free");
    RESOLVE_SYM(set_option, "mpv_set_option");
    RESOLVE_SYM(set_option_string, "mpv_set_option_string");
    RESOLVE_SYM(load_config_file, "mpv_load_config_file");
    RESOLVE_SYM(command, "mpv_command");
    RESOLVE_SYM(command_async, "mpv_command_async");
    RESOLVE_SYM(command_string, "mpv_command_string");
    RESOLVE_SYM(set_property, "mpv_set_property");
    RESOLVE_SYM(set_property_string, "mpv_set_property_string");
    RESOLVE_SYM(set_property_async, "mpv_set_property_async");
    RESOLVE_SYM(get_property, "mpv_get_property");
    RESOLVE_SYM(get_property_string, "mpv_get_property_string");
    RESOLVE_SYM(observe_property, "mpv_observe_property");
    RESOLVE_SYM(set_wakeup_callback, "mpv_set_wakeup_callback");
    RESOLVE_SYM(wait_event, "mpv_wait_event");
    RESOLVE_SYM(render_context_create, "mpv_render_context_create");
    RESOLVE_SYM(render_context_set_update_callback, "mpv_render_context_set_update_callback");
    RESOLVE_SYM(render_context_update, "mpv_render_context_update");
    RESOLVE_SYM(render_context_render, "mpv_render_context_render");
    RESOLVE_SYM(render_context_report_swap, "mpv_render_context_report_swap");
    RESOLVE_SYM(render_context_free, "mpv_render_context_free");
    return true;
}

bool mpv_engine_init(VixMpvEngine *eng, const char *app_base_dir,
                     uint32_t wakeup_event, uint32_t render_event) {
    memset(eng, 0, sizeof(*eng));
    eng->sdl_wakeup_event = wakeup_event;
    eng->sdl_render_event = render_event;
    eng->idle_active = true;
    eng->paused = false;
    eng->volume = 100.0;
    eng->speed = 1.0;
    eng->sub_visible = true;
    eng->ab_loop_a = -1.0;
    eng->ab_loop_b = -1.0;
    eng->playlist_pos = -1;
    snprintf(eng->aid_str, sizeof(eng->aid_str), "auto");
    snprintf(eng->sid_str, sizeof(eng->sid_str), "auto");
    snprintf(eng->hwdec_current, sizeof(eng->hwdec_current), "auto");

    /* libmpv strictly requires C numeric locale */
    setlocale(LC_NUMERIC, "C");

    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(eng->screenshot_dir, sizeof(eng->screenshot_dir), "%s/Pictures/VIXGlitcher", home);
    } else {
        snprintf(eng->screenshot_dir, sizeof(eng->screenshot_dir), "/tmp/VIXGlitcher");
    }
    mkdir_p(eng->screenshot_dir);

    char bundled_mpv_1[VIX_PATH_MAX] = {0};
    char bundled_mpv_2[VIX_PATH_MAX] = {0};
    if (app_base_dir && *app_base_dir) {
        snprintf(bundled_mpv_1, sizeof(bundled_mpv_1), "%s/../lib/libmpv.so.2", app_base_dir);
        snprintf(bundled_mpv_2, sizeof(bundled_mpv_2), "%s/../lib/mpv/libmpv.so.2", app_base_dir);
    }

    const char *env_override = getenv("VIXGLITCHER_LIBMPV");
    const char *candidates[] = {
        env_override,
        "libmpv.so.2",
        "libmpv.so",
        "libmpv.so.1",
        "/usr/lib/libmpv.so.2",
        "/usr/lib/libmpv.so",
        "/usr/local/lib/libmpv.so.2",
        "/usr/lib/x86_64-linux-gnu/libmpv.so.2",
        "/usr/lib64/libmpv.so.2",
        bundled_mpv_1[0] ? bundled_mpv_1 : NULL,
        bundled_mpv_2[0] ? bundled_mpv_2 : NULL,
    };

    void *handle = NULL;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (!candidates[i] || !*candidates[i]) continue;
        handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            if (load_mpv_symbols(handle)) {
                snprintf(eng->libmpv_soname, sizeof(eng->libmpv_soname), "%s", candidates[i]);
                break;
            }
            dlclose(handle);
            handle = NULL;
        }
    }

    if (!handle) {
        eng->libmpv_available = false;
        snprintf(eng->status_error, sizeof(eng->status_error),
                 "libmpv не найден. На Arch Linux выполните: sudo pacman -S mpv");
        return false;
    }

    eng->dl_handle = handle;
    eng->api_version = fn.client_api_version();

    mpv_handle *mpv = fn.create();
    if (!mpv) {
        snprintf(eng->status_error, sizeof(eng->status_error), "Не удалось создать контекст mpv");
        return false;
    }
    eng->mpv = mpv;

    /* Core mpv player options styled in Dark Neon Lilac on #000000 black */
    fn.set_option_string(mpv, "vo", "libmpv");
    fn.set_option_string(mpv, "hwdec", "auto-safe");
    fn.set_option_string(mpv, "keep-open", "yes");
    fn.set_option_string(mpv, "idle", "yes");
    fn.set_option_string(mpv, "ytdl", "yes");
    fn.set_option_string(mpv, "input-default-bindings", "no");
    fn.set_option_string(mpv, "input-vo-keyboard", "no");
    fn.set_option_string(mpv, "osc", "no");
    fn.set_option_string(mpv, "osd-bar", "no");
    fn.set_option_string(mpv, "background-color", "#000000");
    fn.set_option_string(mpv, "volume-max", "150");

    /* Dark Neon Lilac mpv OSD & subtitle styling */
    fn.set_option_string(mpv, "osd-color", "#F3E8FF");
    fn.set_option_string(mpv, "osd-border-color", "#581C87");
    fn.set_option_string(mpv, "osd-back-color", "#CC090412");
    fn.set_option_string(mpv, "osd-border-size", "2");
    fn.set_option_string(mpv, "osd-font-size", "32");
    fn.set_option_string(mpv, "sub-color", "#FAF5FF");
    fn.set_option_string(mpv, "sub-border-color", "#1E0538");
    fn.set_option_string(mpv, "sub-border-size", "2.5");
    fn.set_option_string(mpv, "screenshot-format", "png");
    fn.set_option_string(mpv, "screenshot-directory", eng->screenshot_dir);
    fn.set_option_string(mpv, "screenshot-template", "vixglitcher_%ty%tm%td_%tH%tM%tS_%n");

    /* Load optional user mpv.conf from ~/.config/vixglitcher/mpv.conf */
    if (home && *home) {
        char cfg_path[VIX_PATH_MAX];
        snprintf(cfg_path, sizeof(cfg_path), "%s/.config/vixglitcher/mpv.conf", home);
        struct stat st;
        if (stat(cfg_path, &st) == 0 && S_ISREG(st.st_mode)) {
            fn.load_config_file(mpv, cfg_path);
        }
    }

    int rc = fn.initialize(mpv);
    if (rc < 0) {
        snprintf(eng->status_error, sizeof(eng->status_error),
                 "Ошибка инициализации mpv: %s", fn.error_string(rc));
        fn.terminate_destroy(mpv);
        eng->mpv = NULL;
        return false;
    }

    eng->libmpv_available = true;

    /* Observe live properties */
    fn.observe_property(mpv, 1,  "time-pos",           MPV_FORMAT_DOUBLE);
    fn.observe_property(mpv, 2,  "duration",           MPV_FORMAT_DOUBLE);
    fn.observe_property(mpv, 3,  "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    fn.observe_property(mpv, 4,  "pause",              MPV_FORMAT_FLAG);
    fn.observe_property(mpv, 5,  "idle-active",        MPV_FORMAT_FLAG);
    fn.observe_property(mpv, 6,  "eof-reached",        MPV_FORMAT_FLAG);
    fn.observe_property(mpv, 7,  "volume",             MPV_FORMAT_DOUBLE);
    fn.observe_property(mpv, 8,  "mute",               MPV_FORMAT_FLAG);
    fn.observe_property(mpv, 9,  "speed",              MPV_FORMAT_DOUBLE);
    fn.observe_property(mpv, 10, "media-title",        MPV_FORMAT_STRING);
    fn.observe_property(mpv, 11, "path",               MPV_FORMAT_STRING);
    fn.observe_property(mpv, 12, "video-codec",        MPV_FORMAT_STRING);
    fn.observe_property(mpv, 13, "audio-codec-name",   MPV_FORMAT_STRING);
    fn.observe_property(mpv, 14, "hwdec-current",      MPV_FORMAT_STRING);
    fn.observe_property(mpv, 15, "width",              MPV_FORMAT_INT64);
    fn.observe_property(mpv, 16, "height",             MPV_FORMAT_INT64);
    fn.observe_property(mpv, 17, "container-fps",      MPV_FORMAT_DOUBLE);
    fn.observe_property(mpv, 18, "aid",                MPV_FORMAT_STRING);
    fn.observe_property(mpv, 19, "sid",                MPV_FORMAT_STRING);
    fn.observe_property(mpv, 20, "sub-visibility",     MPV_FORMAT_FLAG);
    fn.observe_property(mpv, 21, "playlist-pos",       MPV_FORMAT_INT64);
    fn.observe_property(mpv, 22, "playlist-count",     MPV_FORMAT_INT64);

    fn.set_wakeup_callback(mpv, on_mpv_wakeup, eng);
    return true;
}

bool mpv_engine_init_gl(VixMpvEngine *eng, void *(*get_proc_address)(void *ctx, const char *name), void *proc_ctx) {
    if (!eng->libmpv_available || !eng->mpv) return false;

    mpv_opengl_init_params gl_init = {
        .get_proc_address = get_proc_address,
        .get_proc_address_ctx = proc_ctx,
    };
    int advanced_control = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };

    mpv_render_context *mpv_gl = NULL;
    int rc = fn.render_context_create(&mpv_gl, (mpv_handle *)eng->mpv, params);
    if (rc < 0 || !mpv_gl) {
        snprintf(eng->status_error, sizeof(eng->status_error),
                 "Ошибка OpenGL контекста mpv: %s", fn.error_string(rc));
        return false;
    }

    eng->mpv_gl = mpv_gl;
    eng->gl_ready = true;
    fn.render_context_set_update_callback(mpv_gl, on_mpv_render_update, eng);
    return true;
}

void mpv_engine_destroy(VixMpvEngine *eng) {
    if (eng->mpv_gl && fn.render_context_free) {
        fn.render_context_free((mpv_render_context *)eng->mpv_gl);
        eng->mpv_gl = NULL;
    }
    if (eng->mpv && fn.terminate_destroy) {
        fn.terminate_destroy((mpv_handle *)eng->mpv);
        eng->mpv = NULL;
    }
    if (eng->dl_handle) {
        dlclose(eng->dl_handle);
        eng->dl_handle = NULL;
    }
    eng->gl_ready = false;
    eng->libmpv_available = false;
}

void mpv_engine_process_events(VixMpvEngine *eng) {
    if (!eng->libmpv_available || !eng->mpv) return;
    mpv_handle *mpv = (mpv_handle *)eng->mpv;

    while (1) {
        mpv_event *ev = fn.wait_event(mpv, 0.0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;

        if (ev->event_id == MPV_EVENT_START_FILE) {
            eng->idle_active = false;
            eng->eof_reached = false;
            eng->time_pos = 0.0;
        } else if (ev->event_id == MPV_EVENT_END_FILE) {
            eng->video_w = 0;
            eng->video_h = 0;
        } else if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *prop = (mpv_event_property *)ev->data;
            if (!prop || !prop->name) continue;

            if (strcmp(prop->name, "time-pos") == 0) {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
                    eng->time_pos = *(double *)prop->data;
            } else if (strcmp(prop->name, "duration") == 0) {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
                    eng->duration = *(double *)prop->data;
                else if (prop->format == MPV_FORMAT_NONE)
                    eng->duration = 0.0;
            } else if (strcmp(prop->name, "demuxer-cache-time") == 0) {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
                    eng->cache_time = *(double *)prop->data;
            } else if (strcmp(prop->name, "pause") == 0) {
                if (prop->format == MPV_FORMAT_FLAG && prop->data)
                    eng->paused = (*(int *)prop->data) != 0;
            } else if (strcmp(prop->name, "idle-active") == 0) {
                if (prop->format == MPV_FORMAT_FLAG && prop->data)
                    eng->idle_active = (*(int *)prop->data) != 0;
            } else if (strcmp(prop->name, "eof-reached") == 0) {
                if (prop->format == MPV_FORMAT_FLAG && prop->data)
                    eng->eof_reached = (*(int *)prop->data) != 0;
            } else if (strcmp(prop->name, "volume") == 0) {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
                    eng->volume = *(double *)prop->data;
            } else if (strcmp(prop->name, "mute") == 0) {
                if (prop->format == MPV_FORMAT_FLAG && prop->data)
                    eng->muted = (*(int *)prop->data) != 0;
            } else if (strcmp(prop->name, "speed") == 0) {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
                    eng->speed = *(double *)prop->data;
            } else if (strcmp(prop->name, "media-title") == 0) {
                if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
                    snprintf(eng->media_title, sizeof(eng->media_title), "%s", *(char **)prop->data);
            } else if (strcmp(prop->name, "path") == 0) {
                if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
                    snprintf(eng->current_path, sizeof(eng->current_path), "%s", *(char **)prop->data);
            } else if (strcmp(prop->name, "video-codec") == 0) {
                if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
                    snprintf(eng->video_codec, sizeof(eng->video_codec), "%s", *(char **)prop->data);
                else
                    eng->video_codec[0] = '\0';
            } else if (strcmp(prop->name, "audio-codec-name") == 0) {
                if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
                    snprintf(eng->audio_codec, sizeof(eng->audio_codec), "%s", *(char **)prop->data);
                else
                    eng->audio_codec[0] = '\0';
            } else if (strcmp(prop->name, "hwdec-current") == 0) {
                if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
                    snprintf(eng->hwdec_current, sizeof(eng->hwdec_current), "%s", *(char **)prop->data);
                else
                    snprintf(eng->hwdec_current, sizeof(eng->hwdec_current), "sw");
            } else if (strcmp(prop->name, "width") == 0) {
                if (prop->format == MPV_FORMAT_INT64 && prop->data)
                    eng->video_w = *(int64_t *)prop->data;
            } else if (strcmp(prop->name, "height") == 0) {
                if (prop->format == MPV_FORMAT_INT64 && prop->data)
                    eng->video_h = *(int64_t *)prop->data;
            } else if (strcmp(prop->name, "container-fps") == 0) {
                if (prop->format == MPV_FORMAT_DOUBLE && prop->data)
                    eng->fps = *(double *)prop->data;
            } else if (strcmp(prop->name, "aid") == 0) {
                if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
                    snprintf(eng->aid_str, sizeof(eng->aid_str), "%s", *(char **)prop->data);
            } else if (strcmp(prop->name, "sid") == 0) {
                if (prop->format == MPV_FORMAT_STRING && prop->data && *(char **)prop->data)
                    snprintf(eng->sid_str, sizeof(eng->sid_str), "%s", *(char **)prop->data);
            } else if (strcmp(prop->name, "sub-visibility") == 0) {
                if (prop->format == MPV_FORMAT_FLAG && prop->data)
                    eng->sub_visible = (*(int *)prop->data) != 0;
            } else if (strcmp(prop->name, "playlist-pos") == 0) {
                if (prop->format == MPV_FORMAT_INT64 && prop->data)
                    eng->playlist_pos = *(int64_t *)prop->data;
                else
                    eng->playlist_pos = -1;
            } else if (strcmp(prop->name, "playlist-count") == 0) {
                if (prop->format == MPV_FORMAT_INT64 && prop->data)
                    eng->playlist_count = *(int64_t *)prop->data;
            }
        }
    }
}

bool mpv_engine_check_render_update(VixMpvEngine *eng) {
    if (!eng->gl_ready || !eng->mpv_gl) return false;
    uint64_t flags = fn.render_context_update((mpv_render_context *)eng->mpv_gl);
    if (flags & MPV_RENDER_UPDATE_FRAME) {
        eng->needs_render = true;
    }
    return eng->needs_render;
}

void mpv_engine_render_gl(VixMpvEngine *eng, int fbo, int width, int height) {
    if (!eng->gl_ready || !eng->mpv_gl) return;

    mpv_opengl_fbo mpfbo = {
        .fbo = fbo,
        .w = width,
        .h = height,
        .internal_format = 0,
    };
    int flip_y = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };
    fn.render_context_render((mpv_render_context *)eng->mpv_gl, params);
    eng->needs_render = false;
}

void mpv_engine_report_swap(VixMpvEngine *eng) {
    if (!eng->gl_ready || !eng->mpv_gl) return;
    fn.render_context_report_swap((mpv_render_context *)eng->mpv_gl);
}

bool mpv_engine_load(VixMpvEngine *eng, const char *path_or_url, bool append_to_playlist) {
    if (!path_or_url || !*path_or_url) return false;

    bool should_append = append_to_playlist && (eng->item_count > 0) && !eng->idle_active;

    if (!should_append) {
        eng->item_count = 0;
    }
    if (eng->item_count < VIX_MAX_PLAYLIST) {
        int idx = eng->item_count++;
        snprintf(eng->items[idx].path, sizeof(eng->items[idx].path), "%s", path_or_url);
        extract_display_title(path_or_url, eng->items[idx].title, sizeof(eng->items[idx].title));
        if (!should_append) {
            eng->playlist_pos = 0;
            snprintf(eng->media_title, sizeof(eng->media_title), "%s", eng->items[idx].title);
            snprintf(eng->current_path, sizeof(eng->current_path), "%s", path_or_url);
        }
    }

    if (!eng->libmpv_available || !eng->mpv) return false;

    const char *mode = should_append ? "append-play" : "replace";
    const char *cmd[] = {"loadfile", path_or_url, mode, NULL};
    int rc = fn.command((mpv_handle *)eng->mpv, cmd);
    if (rc >= 0 && !should_append) {
        int pause_flag = 0;
        fn.set_property((mpv_handle *)eng->mpv, "pause", MPV_FORMAT_FLAG, &pause_flag);
        eng->idle_active = false;
    }
    return rc >= 0;
}

void mpv_engine_play_index(VixMpvEngine *eng, int index) {
    if (index < 0 || index >= eng->item_count) return;
    eng->playlist_pos = index;
    snprintf(eng->media_title, sizeof(eng->media_title), "%s", eng->items[index].title);
    if (!eng->libmpv_available || !eng->mpv) return;

    int64_t pos = index;
    fn.set_property((mpv_handle *)eng->mpv, "playlist-pos", MPV_FORMAT_INT64, &pos);
    int pause_flag = 0;
    fn.set_property((mpv_handle *)eng->mpv, "pause", MPV_FORMAT_FLAG, &pause_flag);
}

void mpv_engine_remove_index(VixMpvEngine *eng, int index) {
    if (index < 0 || index >= eng->item_count) return;

    if (eng->libmpv_available && eng->mpv) {
        char idx_str[32];
        snprintf(idx_str, sizeof(idx_str), "%d", index);
        const char *cmd[] = {"playlist-remove", idx_str, NULL};
        fn.command((mpv_handle *)eng->mpv, cmd);
    }

    for (int i = index; i < eng->item_count - 1; i++) {
        eng->items[i] = eng->items[i + 1];
    }
    eng->item_count--;
    if (eng->item_count == 0) {
        eng->playlist_pos = -1;
        eng->idle_active = true;
    } else if (eng->playlist_pos >= eng->item_count) {
        eng->playlist_pos = eng->item_count - 1;
    }
}

void mpv_engine_clear_playlist(VixMpvEngine *eng) {
    eng->item_count = 0;
    eng->playlist_pos = -1;
    mpv_engine_stop(eng);
}

void mpv_engine_prev(VixMpvEngine *eng) {
    if (!eng->libmpv_available || !eng->mpv) return;
    const char *cmd[] = {"playlist-prev", "weak", NULL};
    fn.command_async((mpv_handle *)eng->mpv, 0, cmd);
}

void mpv_engine_next(VixMpvEngine *eng) {
    if (!eng->libmpv_available || !eng->mpv) return;
    const char *cmd[] = {"playlist-next", "weak", NULL};
    fn.command_async((mpv_handle *)eng->mpv, 0, cmd);
}

void mpv_engine_toggle_pause(VixMpvEngine *eng) {
    if (!eng->libmpv_available || !eng->mpv) {
        eng->paused = !eng->paused;
        return;
    }
    if (eng->idle_active && eng->item_count > 0) {
        mpv_engine_play_index(eng, 0);
        return;
    }
    if (eng->eof_reached && !eng->idle_active) {
        mpv_engine_seek_absolute(eng, 0.0);
        mpv_engine_set_pause(eng, false);
        return;
    }
    mpv_engine_set_pause(eng, !eng->paused);
}

void mpv_engine_set_pause(VixMpvEngine *eng, bool pause) {
    eng->paused = pause;
    if (!eng->libmpv_available || !eng->mpv) return;
    int flag = pause ? 1 : 0;
    fn.set_property_async((mpv_handle *)eng->mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
}

void mpv_engine_stop(VixMpvEngine *eng) {
    eng->idle_active = true;
    eng->time_pos = 0.0;
    eng->duration = 0.0;
    eng->media_title[0] = '\0';
    if (!eng->libmpv_available || !eng->mpv) return;
    const char *cmd[] = {"stop", NULL};
    fn.command((mpv_handle *)eng->mpv, cmd);
}

void mpv_engine_seek_relative(VixMpvEngine *eng, double seconds) {
    if (!eng->libmpv_available || !eng->mpv || eng->idle_active) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", seconds);
    const char *cmd[] = {"seek", buf, "relative", NULL};
    fn.command_async((mpv_handle *)eng->mpv, 0, cmd);
}

void mpv_engine_seek_absolute(VixMpvEngine *eng, double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    eng->time_pos = seconds;
    if (!eng->libmpv_available || !eng->mpv || eng->idle_active) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.3f", seconds);
    const char *cmd[] = {"seek", buf, "absolute", NULL};
    fn.command_async((mpv_handle *)eng->mpv, 0, cmd);
}

void mpv_engine_frame_step(VixMpvEngine *eng, bool backward) {
    if (!eng->libmpv_available || !eng->mpv || eng->idle_active) return;
    const char *cmd[] = {backward ? "frame-back-step" : "frame-step", NULL};
    fn.command_async((mpv_handle *)eng->mpv, 0, cmd);
}

void mpv_engine_set_volume(VixMpvEngine *eng, double vol) {
    if (vol < 0.0) vol = 0.0;
    if (vol > 150.0) vol = 150.0;
    eng->volume = vol;
    if (!eng->libmpv_available || !eng->mpv) return;
    fn.set_property_async((mpv_handle *)eng->mpv, 0, "volume", MPV_FORMAT_DOUBLE, &eng->volume);
}

void mpv_engine_add_volume(VixMpvEngine *eng, double delta) {
    mpv_engine_set_volume(eng, eng->volume + delta);
}

void mpv_engine_toggle_mute(VixMpvEngine *eng) {
    eng->muted = !eng->muted;
    if (!eng->libmpv_available || !eng->mpv) return;
    int flag = eng->muted ? 1 : 0;
    fn.set_property_async((mpv_handle *)eng->mpv, 0, "mute", MPV_FORMAT_FLAG, &flag);
}

void mpv_engine_set_speed(VixMpvEngine *eng, double speed) {
    if (speed < 0.25) speed = 0.25;
    if (speed > 4.0) speed = 4.0;
    eng->speed = speed;
    if (!eng->libmpv_available || !eng->mpv) return;
    fn.set_property_async((mpv_handle *)eng->mpv, 0, "speed", MPV_FORMAT_DOUBLE, &eng->speed);
}

void mpv_engine_cycle_speed(VixMpvEngine *eng, int direction) {
    static const double presets[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
    const int n = (int)(sizeof(presets) / sizeof(presets[0]));
    int best = 2;
    double best_diff = 999.0;
    for (int i = 0; i < n; i++) {
        double d = fabs(eng->speed - presets[i]);
        if (d < best_diff) {
            best_diff = d;
            best = i;
        }
    }
    int next = (best + direction + n) % n;
    mpv_engine_set_speed(eng, presets[next]);
}

void mpv_engine_cycle_audio(VixMpvEngine *eng) {
    if (!eng->libmpv_available || !eng->mpv) return;
    const char *cmd[] = {"cycle", "aid", NULL};
    fn.command_async((mpv_handle *)eng->mpv, 0, cmd);
}

void mpv_engine_cycle_sub(VixMpvEngine *eng) {
    if (!eng->libmpv_available || !eng->mpv) return;
    const char *cmd[] = {"cycle", "sid", NULL};
    fn.command_async((mpv_handle *)eng->mpv, 0, cmd);
}

void mpv_engine_toggle_sub_visibility(VixMpvEngine *eng) {
    eng->sub_visible = !eng->sub_visible;
    if (!eng->libmpv_available || !eng->mpv) return;
    int flag = eng->sub_visible ? 1 : 0;
    fn.set_property_async((mpv_handle *)eng->mpv, 0, "sub-visibility", MPV_FORMAT_FLAG, &flag);
}

bool mpv_engine_load_subtitle(VixMpvEngine *eng, const char *sub_path) {
    if (!eng->libmpv_available || !eng->mpv || !sub_path || !*sub_path) return false;
    const char *cmd[] = {"sub-add", sub_path, "select", NULL};
    return fn.command((mpv_handle *)eng->mpv, cmd) >= 0;
}

const char *mpv_engine_fx_name(VixGlitchPreset preset) {
    switch (preset) {
        case VIX_FX_OFF:         return "OFF";
        case VIX_FX_RGB_GLITCH:  return "RGB GLITCH";
        case VIX_FX_NEON_LILAC:  return "NEON LILAC";
        case VIX_FX_CRT_SCAN:    return "CRT SCAN";
        case VIX_FX_CYBER_BOOST: return "CYBER BOOST";
        default:                 return "OFF";
    }
}

void mpv_engine_set_fx(VixMpvEngine *eng, VixGlitchPreset preset) {
    eng->active_fx = (VixGlitchPreset)(preset % VIX_FX_COUNT);
    if (!eng->libmpv_available || !eng->mpv) return;

    const char *vf_expr = "";
    switch (eng->active_fx) {
        case VIX_FX_OFF:
            vf_expr = "";
            break;
        case VIX_FX_RGB_GLITCH:
            vf_expr = "lavfi=[rgbashift=rh=-5:bh=5:rv=1:bv=-1]";
            break;
        case VIX_FX_NEON_LILAC:
            vf_expr = "lavfi=[colorbalance=rs=0.07:bs=0.22:rm=0.05:bm=0.16:rh=0.08:bh=0.20,eq=contrast=1.08:saturation=1.20]";
            break;
        case VIX_FX_CRT_SCAN:
            vf_expr = "lavfi=[drawgrid=w=iw:h=4:t=1:c=black@0.35,rgbashift=rh=-2:bh=2]";
            break;
        case VIX_FX_CYBER_BOOST:
            vf_expr = "lavfi=[eq=contrast=1.16:brightness=0.02:saturation=1.24]";
            break;
        default:
            break;
    }
    fn.set_property_string((mpv_handle *)eng->mpv, "vf", vf_expr);
}

void mpv_engine_cycle_fx(VixMpvEngine *eng) {
    mpv_engine_set_fx(eng, (VixGlitchPreset)((eng->active_fx + 1) % VIX_FX_COUNT));
}

bool mpv_engine_screenshot(VixMpvEngine *eng, char *out_msg, int out_msg_len) {
    if (!eng->libmpv_available || !eng->mpv || eng->idle_active) {
        if (out_msg) snprintf(out_msg, out_msg_len, "Нет активного видео для скриншота");
        return false;
    }
    const char *cmd[] = {"screenshot", "video", NULL};
    int rc = fn.command((mpv_handle *)eng->mpv, cmd);
    if (rc >= 0) {
        if (out_msg) snprintf(out_msg, out_msg_len, "Скриншот -> %s", eng->screenshot_dir);
        return true;
    }
    if (out_msg) snprintf(out_msg, out_msg_len, "Ошибка скриншота: %s", fn.error_string(rc));
    return false;
}

void mpv_engine_ab_loop(VixMpvEngine *eng, char *out_msg, int out_msg_len) {
    if (!eng->libmpv_available || !eng->mpv || eng->idle_active) return;
    const char *cmd[] = {"ab-loop", NULL};
    fn.command((mpv_handle *)eng->mpv, cmd);

    char *a = fn.get_property_string((mpv_handle *)eng->mpv, "ab-loop-a");
    char *b = fn.get_property_string((mpv_handle *)eng->mpv, "ab-loop-b");
    const char *as = (a && strcmp(a, "no") != 0) ? a : NULL;
    const char *bs = (b && strcmp(b, "no") != 0) ? b : NULL;

    if (as && bs) {
        eng->ab_loop_a = atof(as);
        eng->ab_loop_b = atof(bs);
        if (out_msg) snprintf(out_msg, out_msg_len, "A-B повтор: %.1fs -> %.1fs", eng->ab_loop_a, eng->ab_loop_b);
    } else if (as) {
        eng->ab_loop_a = atof(as);
        eng->ab_loop_b = -1.0;
        if (out_msg) snprintf(out_msg, out_msg_len, "Точка A установлена: %.1fs", eng->ab_loop_a);
    } else {
        eng->ab_loop_a = -1.0;
        eng->ab_loop_b = -1.0;
        if (out_msg) snprintf(out_msg, out_msg_len, "A-B повтор сброшен");
    }
    if (a) fn.free(a);
    if (b) fn.free(b);
}

void mpv_engine_adjust_eq(VixMpvEngine *eng, const char *prop, int delta) {
    if (!eng->libmpv_available || !eng->mpv || !prop) return;
    char dstr[16];
    snprintf(dstr, sizeof(dstr), "%d", delta);
    const char *cmd[] = {"add", prop, dstr, NULL};
    fn.command_async((mpv_handle *)eng->mpv, 0, cmd);
}
