#define _GNU_SOURCE
#include "gl_loader.h"
#include "mpv_engine.h"
#include "player_ui.h"
#include "ui_renderer.h"

#include <SDL.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VIXGLITCHER_VERSION "1.0.0"

static void get_executable_dir(char *out, size_t out_sz) {
    char buf[2048] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        char *slash = strrchr(buf, '/');
        if (slash) *slash = '\0';
        snprintf(out, out_sz, "%s", buf);
    } else {
        snprintf(out, out_sz, ".");
    }
}

static void *sdl_get_proc_for_mpv(void *ctx, const char *name) {
    (void)ctx;
    return SDL_GL_GetProcAddress(name);
}

static void set_window_app_icon(SDL_Window *win, const char *base_dir) {
    const char *rel_paths[] = {
        "assets/icons/vixglitcher-256.png",
        "../share/vixglitcher/icons/vixglitcher-256.png",
        "../share/icons/hicolor/256x256/apps/vixglitcher.png",
        "/usr/share/icons/hicolor/256x256/apps/vixglitcher.png",
        NULL
    };
    for (int i = 0; rel_paths[i]; i++) {
        char path[2560];
        if (rel_paths[i][0] == '/')
            snprintf(path, sizeof(path), "%s", rel_paths[i]);
        else
            snprintf(path, sizeof(path), "%s/%s", base_dir, rel_paths[i]);

        int iw = 0, ih = 0;
        uint8_t *rgba = ui_load_rgba_file(path, &iw, &ih);
        if (rgba && iw > 0 && ih > 0) {
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
                rgba, iw, ih, 32, iw * 4, SDL_PIXELFORMAT_RGBA32);
            if (surf) {
                SDL_SetWindowIcon(win, surf);
                SDL_FreeSurface(surf);
            }
            ui_free_rgba_file(rgba);
            return;
        }
    }
}

static int run_headless_ui_preview(const char *base_dir, const char *out_prefix) {
    VixUiRenderer *r = ui_renderer_create(VIX_BACKEND_SOFTWARE, base_dir);
    if (!r) return 1;

    VixMpvEngine eng;
    mpv_engine_init(&eng, base_dir, 0, 0);
    /* Show clean showcase state for preview */
    eng.libmpv_available = true;
    snprintf(eng.libmpv_soname, sizeof(eng.libmpv_soname), "libmpv.so.2");
    eng.api_version = (2UL << 16) | 3UL;

    VixPlayerUi ui;
    player_ui_init(&ui, 1280, 760);
    ui.anim_time = 0.8;

    /* 1. Idle showcase on #000000 black */
    char p1[2048], p2[2048], p3[2048];
    snprintf(p1, sizeof(p1), "%s_idle.png", out_prefix);
    snprintf(p2, sizeof(p2), "%s_player.png", out_prefix);
    snprintf(p3, sizeof(p3), "%s_browser.png", out_prefix);

    ui_begin_frame(r, 1280, 760);
    ui_clear(r, VIX_COL_BLACK);
    player_ui_render(&ui, r, &eng);
    ui_end_frame(r);
    ui_save_png(r, p1);

    /* 2. Active playback + HUD + Info + Playlist showcase */
    eng.idle_active = false;
    eng.paused = false;
    eng.duration = 1432.0;
    eng.time_pos = 418.0;
    eng.cache_time = 120.0;
    eng.volume = 100.0;
    eng.speed = 1.0;
    eng.video_w = 1920;
    eng.video_h = 1080;
    eng.fps = 60.0;
    eng.active_fx = VIX_FX_NEON_LILAC;
    snprintf(eng.media_title, sizeof(eng.media_title), "Cyberpunk_Night_Session_ArchLinux_1080p60.mkv");
    snprintf(eng.video_codec, sizeof(eng.video_codec), "hevc (Main 10)");
    snprintf(eng.audio_codec, sizeof(eng.audio_codec), "opus");
    snprintf(eng.hwdec_current, sizeof(eng.hwdec_current), "vaapi");
    eng.item_count = 3;
    eng.playlist_pos = 0;
    snprintf(eng.items[0].title, sizeof(eng.items[0].title), "Cyberpunk_Night_Session_ArchLinux_1080p60.mkv");
    snprintf(eng.items[1].title, sizeof(eng.items[1].title), "Glitch_Synthwave_Live_Mix.webm");
    snprintf(eng.items[2].title, sizeof(eng.items[2].title), "Neon_Forest_Visualizer_4K.mp4");
    ui.show_info_hud = true;
    ui.show_playlist = true;
    ui.mouse_x = 460.0f;
    ui.mouse_y = 760.0f - 84.0f + 16.0f;
    player_ui_show_toast(&ui, "VIXGlitch FX: NEON LILAC  •  mpv hwdec=vaapi");

    ui_begin_frame(r, 1280, 760);
    ui_clear(r, VIX_COL_BLACK);
    player_ui_render(&ui, r, &eng);
    ui_end_frame(r);
    ui_save_png(r, p2);

    /* 3. Built-in Dark Neon Lilac File Browser showcase */
    ui.show_info_hud = false;
    ui.show_playlist = false;
    ui.toast_until = 0.0;
    player_ui_open_file_modal(&ui, false);
    ui_begin_frame(r, 1280, 760);
    ui_clear(r, VIX_COL_BLACK);
    player_ui_render(&ui, r, &eng);
    ui_end_frame(r);
    ui_save_png(r, p3);

    mpv_engine_destroy(&eng);
    ui_renderer_destroy(r);
    return 0;
}

static void print_help(const char *prog) {
    printf("VIXGlitcher v%s — Dark Neon Lilac mpv Video Player for Arch Linux\n\n", VIXGLITCHER_VERSION);
    printf("Использование / Usage:\n");
    printf("  %s [опции] [файл_или_URL ...]\n\n", prog);
    printf("Опции / Options:\n");
    printf("  -f, --fullscreen          Запуск в полноэкранном режиме\n");
    printf("  --fx <0..4>               Предустановка VIXGlitch FX (0=OFF, 1=RGB, 2=LILAC, 3=CRT, 4=BOOST)\n");
    printf("  -v, --version             Показать версию VIXGlitcher\n");
    printf("  -h, --help                Показать эту справку\n");
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");

    char base_dir[2048];
    get_executable_dir(base_dir, sizeof(base_dir));

    bool start_fullscreen = false;
    int initial_fx = 0;
    const char *preview_prefix = NULL;
    const char *initial_files[VIX_MAX_PLAYLIST];
    int initial_file_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("VIXGlitcher %s (Arch Linux x86_64 / libmpv engine)\n", VIXGLITCHER_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--fullscreen") == 0 || strcmp(argv[i], "-f") == 0) {
            start_fullscreen = true;
            continue;
        }
        if (strcmp(argv[i], "--fx") == 0 && i + 1 < argc) {
            initial_fx = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--dump-ui-preview") == 0 && i + 1 < argc) {
            preview_prefix = argv[++i];
            continue;
        }
        if (argv[i][0] != '-' && initial_file_count < VIX_MAX_PLAYLIST) {
            initial_files[initial_file_count++] = argv[i];
        }
    }

    if (preview_prefix) {
        return run_headless_ui_preview(base_dir, preview_prefix);
    }

    /* Hints for native Arch Linux Wayland (Hyprland/KDE/GNOME/Sway) & X11 */
    SDL_SetHint(SDL_HINT_APP_NAME, "VIXGlitcher");
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "vixglitcher");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

    /* Prefer Wayland automatically on pure Wayland sessions if DISPLAY is unset */
    if (!getenv("SDL_VIDEODRIVER") && getenv("WAYLAND_DISPLAY") && !getenv("DISPLAY")) {
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[VIXGlitcher] Ошибка инициализации видеоподсистемы SDL2: %s\n", SDL_GetError());
        return 1;
    }

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    /* Attempt OpenGL 3.2 Core -> OpenGL 2.1 Compatibility -> Software Window */
    SDL_Window *win = NULL;
    SDL_GLContext gl_ctx = NULL;
    bool using_opengl = false;
    bool core_profile = false;

    const int init_w = 1280;
    const int init_h = 760;

    /* Try 1: OpenGL 3.2 Core */
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    win = SDL_CreateWindow("VIXGlitcher",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           init_w, init_h,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (win) {
        gl_ctx = SDL_GL_CreateContext(win);
        if (gl_ctx && vix_gl_load(SDL_GL_GetProcAddress, true)) {
            using_opengl = true;
            core_profile = true;
        } else {
            if (gl_ctx) { SDL_GL_DeleteContext(gl_ctx); gl_ctx = NULL; }
            SDL_DestroyWindow(win);
            win = NULL;
        }
    }

    /* Try 2: OpenGL 2.1 Compatibility */
    if (!using_opengl) {
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

        win = SDL_CreateWindow("VIXGlitcher",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               init_w, init_h,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (win) {
            gl_ctx = SDL_GL_CreateContext(win);
            if (gl_ctx && vix_gl_load(SDL_GL_GetProcAddress, false)) {
                using_opengl = true;
                core_profile = false;
            } else {
                if (gl_ctx) { SDL_GL_DeleteContext(gl_ctx); gl_ctx = NULL; }
                SDL_DestroyWindow(win);
                win = NULL;
            }
        }
    }

    /* Try 3: Pure Software Window Fallback */
    if (!using_opengl) {
        win = SDL_CreateWindow("VIXGlitcher",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               init_w, init_h,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!win) {
            fprintf(stderr, "[VIXGlitcher] Не удалось создать окно: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }
    }
    (void)core_profile;

    SDL_SetWindowMinimumSize(win, 640, 400);
    set_window_app_icon(win, base_dir);
    if (using_opengl) {
        SDL_GL_SetSwapInterval(1);
    }

    uint32_t ev_base = SDL_RegisterEvents(2);
    uint32_t ev_mpv_wakeup = (ev_base != (uint32_t)-1) ? ev_base : 0;
    uint32_t ev_mpv_render = (ev_base != (uint32_t)-1) ? (ev_base + 1) : 0;

    VixMpvEngine eng;
    mpv_engine_init(&eng, base_dir, ev_mpv_wakeup, ev_mpv_render);
    if (using_opengl && eng.libmpv_available) {
        mpv_engine_init_gl(&eng, sdl_get_proc_for_mpv, NULL);
    }
    if (initial_fx > 0) {
        mpv_engine_set_fx(&eng, (VixGlitchPreset)initial_fx);
    }

    VixUiRenderer *ui_ren = ui_renderer_create(
        using_opengl ? VIX_BACKEND_OPENGL : VIX_BACKEND_SOFTWARE, base_dir);

    VixPlayerUi ui;
    player_ui_init(&ui, init_w, init_h);

    if (start_fullscreen) {
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        ui.is_fullscreen = true;
    }

    for (int i = 0; i < initial_file_count; i++) {
        mpv_engine_load(&eng, initial_files[i], i > 0);
    }

    char prev_win_title[512] = "VIXGlitcher";
    uint64_t last_counter = SDL_GetPerformanceCounter();
    double perf_freq = (double)SDL_GetPerformanceFrequency();

    while (!ui.request_quit) {
        /* Wait efficiently: 16ms during active playback/animations, 50ms when static */
        int wait_ms = (!eng.idle_active && !eng.paused) ? 8 : 24;
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, wait_ms)) {
            do {
                if (ev.type == SDL_QUIT) {
                    ui.request_quit = true;
                } else if (ev_mpv_wakeup != 0 && ev.type == ev_mpv_wakeup) {
                    mpv_engine_process_events(&eng);
                } else if (ev_mpv_render != 0 && ev.type == ev_mpv_render) {
                    mpv_engine_check_render_update(&eng);
                } else {
                    player_ui_handle_event(&ui, &ev, &eng);
                }
            } while (SDL_PollEvent(&ev));
        }

        mpv_engine_process_events(&eng);
        mpv_engine_check_render_update(&eng);

        if (ui.request_toggle_fullscreen) {
            ui.request_toggle_fullscreen = false;
            ui.is_fullscreen = !ui.is_fullscreen;
            SDL_SetWindowFullscreen(win, ui.is_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }

        /* Sync window title with active media */
        char desired_title[512];
        if (!eng.idle_active && eng.media_title[0]) {
            snprintf(desired_title, sizeof(desired_title), "VIXGlitcher — %s", eng.media_title);
        } else {
            snprintf(desired_title, sizeof(desired_title), "VIXGlitcher");
        }
        if (strcmp(desired_title, prev_win_title) != 0) {
            SDL_SetWindowTitle(win, desired_title);
            snprintf(prev_win_title, sizeof(prev_win_title), "%s", desired_title);
        }

        uint64_t now_counter = SDL_GetPerformanceCounter();
        double dt = (double)(now_counter - last_counter) / perf_freq;
        if (dt > 0.1) dt = 0.1;
        last_counter = now_counter;

        player_ui_update(&ui, dt, &eng);

        int dw = init_w, dh = init_h;
        if (using_opengl) {
            SDL_GL_GetDrawableSize(win, &dw, &dh);
        } else {
            SDL_GetWindowSize(win, &dw, &dh);
        }
        ui.win_w = dw;
        ui.win_h = dh;

        /* Hide OS cursor when HUD is auto-hidden in fullscreen/playback */
        bool show_cursor = (ui.hud_alpha > 0.05f) || (ui.modal != VIX_MODAL_NONE);
        SDL_ShowCursor(show_cursor ? SDL_ENABLE : SDL_DISABLE);

        /* Render Frame: Pure #000000 Black + libmpv Video + Dark Neon Lilac UI */
        ui_begin_frame(ui_ren, dw, dh);
        ui_clear(ui_ren, VIX_COL_BLACK);

        if (using_opengl && !eng.idle_active && eng.gl_ready) {
            mpv_engine_render_gl(&eng, 0, dw, dh);
            /* Restore UI OpenGL state after libmpv render */
            ui_begin_frame(ui_ren, dw, dh);
        }

        player_ui_render(&ui, ui_ren, &eng);
        ui_end_frame(ui_ren);

        if (using_opengl) {
            SDL_GL_SwapWindow(win);
            if (!eng.idle_active && eng.gl_ready) {
                mpv_engine_report_swap(&eng);
            }
        } else {
            SDL_Surface *ws = SDL_GetWindowSurface(win);
            int sw = 0, sh = 0;
            const uint8_t *px = ui_software_pixels(ui_ren, &sw, &sh);
            if (ws && px && sw == ws->w && sh == ws->h) {
                SDL_Surface *src = SDL_CreateRGBSurfaceWithFormatFrom(
                    (void *)px, sw, sh, 32, sw * 4, SDL_PIXELFORMAT_RGBA32);
                if (src) {
                    SDL_BlitSurface(src, NULL, ws, NULL);
                    SDL_FreeSurface(src);
                    SDL_UpdateWindowSurface(win);
                }
            }
        }
    }

    SDL_ShowCursor(SDL_ENABLE);
    ui_renderer_destroy(ui_ren);
    mpv_engine_destroy(&eng);
    if (gl_ctx) SDL_GL_DeleteContext(gl_ctx);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
