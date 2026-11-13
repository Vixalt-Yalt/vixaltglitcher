#define _GNU_SOURCE
#include "player_ui.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOP_BAR_H    46.0f
#define BOT_DECK_H   84.0f
#define DRAWER_W    320.0f

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static void format_timecode(double secs, char *out, size_t sz) {
    if (secs < 0.0 || isnan(secs) || isinf(secs)) secs = 0.0;
    int total = (int)(secs + 0.5);
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    if (h > 0) {
        snprintf(out, sz, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(out, sz, "%02d:%02d", m, s);
    }
}

static void format_filesize(uint64_t bytes, char *out, size_t sz) {
    if (bytes >= (1024ULL * 1024ULL * 1024ULL)) {
        snprintf(out, sz, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= (1024ULL * 1024ULL)) {
        snprintf(out, sz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(out, sz, "%.0f KB", (double)bytes / 1024.0);
    } else {
        snprintf(out, sz, "%llu B", (unsigned long long)bytes);
    }
}

static bool has_extension(const char *name, const char *const *exts) {
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return false;
    dot++;
    for (int i = 0; exts[i]; i++) {
        if (strcasecmp(dot, exts[i]) == 0) return true;
    }
    return false;
}

static bool is_subtitle_filename(const char *name) {
    static const char *const sub_exts[] = {
        "srt", "ass", "ssa", "vtt", "sub", "idx", "sup", NULL
    };
    return has_extension(name, sub_exts);
}

static bool is_media_filename(const char *name) {
    static const char *const media_exts[] = {
        "mp4", "mkv", "webm", "avi", "mov", "m4v", "flv", "ts", "m2ts",
        "mpg", "mpeg", "wmv", "ogv", "3gp", "vob",
        "mp3", "flac", "ogg", "opus", "wav", "m4a", "aac", "wma",
        "m3u", "m3u8", "pls", NULL
    };
    return has_extension(name, media_exts) || is_subtitle_filename(name);
}

static int cmp_dir_entry(const void *a, const void *b) {
    const VixDirEntry *ea = (const VixDirEntry *)a;
    const VixDirEntry *eb = (const VixDirEntry *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcasecmp(ea->name, eb->name);
}

static void refresh_browser_dir(VixPlayerUi *ui) {
    ui->dir_entry_count = 0;
    ui->selected_entry = -1;
    ui->browser_scroll = 0.0f;

    DIR *d = opendir(ui->browser_dir);
    if (!d) {
        snprintf(ui->browser_dir, sizeof(ui->browser_dir), "/");
        d = opendir("/");
        if (!d) return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && ui->dir_entry_count < VIX_MAX_DIR_ENTRIES) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (ent->d_name[0] == '.' && !ui->show_all_files) continue;

        char full[VIX_PATH_MAX + 512];
        if (strcmp(ui->browser_dir, "/") == 0)
            snprintf(full, sizeof(full), "/%s", ent->d_name);
        else
            snprintf(full, sizeof(full), "%s/%s", ui->browser_dir, ent->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        bool is_dir = S_ISDIR(st.st_mode);
        bool is_sub = !is_dir && is_subtitle_filename(ent->d_name);
        if (!is_dir && !ui->show_all_files) {
            if (ui->subtitle_pick_mode) {
                if (!is_sub) continue;
            } else {
                if (!is_media_filename(ent->d_name)) continue;
            }
        }

        VixDirEntry *dst = &ui->dir_entries[ui->dir_entry_count++];
        snprintf(dst->name, sizeof(dst->name), "%s", ent->d_name);
        dst->is_dir = is_dir;
        dst->is_subtitle = is_sub;
        dst->size_bytes = is_dir ? 0 : (uint64_t)st.st_size;
    }
    closedir(d);

    if (ui->dir_entry_count > 1) {
        qsort(ui->dir_entries, (size_t)ui->dir_entry_count, sizeof(VixDirEntry), cmp_dir_entry);
    }
}

static void navigate_parent_dir(VixPlayerUi *ui) {
    size_t len = strlen(ui->browser_dir);
    while (len > 1 && ui->browser_dir[len - 1] == '/') {
        ui->browser_dir[--len] = '\0';
    }
    char *slash = strrchr(ui->browser_dir, '/');
    if (!slash || slash == ui->browser_dir) {
        snprintf(ui->browser_dir, sizeof(ui->browser_dir), "/");
    } else {
        *slash = '\0';
    }
    refresh_browser_dir(ui);
}

static bool try_native_file_dialog(char *out_path, size_t out_sz) {
    const char *cmds[] = {
        "zenity --file-selection --title='VIXGlitcher — Открыть медиафайл' 2>/dev/null",
        "kdialog --getopenfilename . '*.mp4 *.mkv *.webm *.avi *.mov *.mp3 *.flac *.srt *.ass|Media Files' 2>/dev/null",
        NULL
    };
    const char *bins[] = { "/usr/bin/zenity", "/usr/bin/kdialog", NULL };
    for (int i = 0; cmds[i]; i++) {
        if (access(bins[i], X_OK) != 0) continue;
        FILE *fp = popen(cmds[i], "r");
        if (!fp) continue;
        char buf[VIX_PATH_MAX] = {0};
        if (fgets(buf, sizeof(buf), fp) != NULL) {
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
            int status = pclose(fp);
            if (status == 0 && l > 0) {
                snprintf(out_path, out_sz, "%s", buf);
                return true;
            }
            return false;
        }
        pclose(fp);
        return false;
    }
    return false;
}

static void utf8_pop_last_char(char *s) {
    size_t len = strlen(s);
    if (len == 0) return;
    while (len > 0 && ((unsigned char)s[len - 1] & 0xC0) == 0x80) {
        len--;
    }
    if (len > 0) len--;
    s[len] = '\0';
}

/* ---------------- Shared Layout Geometry ---------------- */

typedef struct HudLayout {
    VixRect top_bar;
    VixRect btn_top_open;
    VixRect btn_top_url;
    VixRect btn_top_info;
    VixRect btn_top_list;

    VixRect bot_deck;
    VixRect timeline_hit;
    VixRect timeline_bar;

    VixRect btn_prev;
    VixRect btn_rew;
    VixRect btn_play;
    VixRect btn_fwd;
    VixRect btn_next;
    VixRect btn_stop;
    VixRect time_box;

    VixRect btn_fs;
    VixRect btn_snap;
    VixRect btn_fx;
    VixRect btn_sub;
    VixRect btn_aud;
    VixRect btn_speed;
    VixRect vol_slider_hit;
    VixRect vol_slider_bar;
    VixRect btn_mute;

    /* Idle screen buttons */
    VixRect idle_card;
    VixRect btn_idle_open;
    VixRect btn_idle_url;
    VixRect btn_idle_copy_cmd;

    /* Playlist drawer */
    VixRect drawer;
    VixRect btn_pl_add;
    VixRect btn_pl_clear;
    VixRect btn_pl_close;
    VixRect pl_list_area;
} HudLayout;

static void compute_hud_layout(const VixPlayerUi *ui, HudLayout *L) {
    memset(L, 0, sizeof(*L));
    float W = (float)ui->win_w;
    float H = (float)ui->win_h;

    /* Top Bar */
    L->top_bar = (VixRect){0, 0, W, TOP_BAR_H};
    float rx = W - 14.0f;
    L->btn_top_list = (VixRect){rx - 108.0f, 8.0f, 108.0f, 30.0f}; rx -= 116.0f;
    L->btn_top_info = (VixRect){rx - 66.0f,  8.0f,  66.0f, 30.0f}; rx -= 74.0f;
    L->btn_top_url  = (VixRect){rx - 58.0f,  8.0f,  58.0f, 30.0f}; rx -= 66.0f;
    L->btn_top_open = (VixRect){rx - 72.0f,  8.0f,  72.0f, 30.0f};

    /* Bottom Control Deck */
    L->bot_deck = (VixRect){0, H - BOT_DECK_H, W, BOT_DECK_H};
    L->timeline_hit = (VixRect){20.0f, H - BOT_DECK_H + 8.0f, fmaxf(80.0f, W - 40.0f), 20.0f};
    L->timeline_bar = (VixRect){20.0f, H - BOT_DECK_H + 15.0f, fmaxf(80.0f, W - 40.0f), 6.0f};

    float cy = H - 46.0f;
    float lx = 18.0f;
    L->btn_prev = (VixRect){lx, cy, 32.0f, 32.0f}; lx += 38.0f;
    L->btn_rew  = (VixRect){lx, cy, 32.0f, 32.0f}; lx += 40.0f;
    L->btn_play = (VixRect){lx, cy - 3.0f, 44.0f, 38.0f}; lx += 52.0f;
    L->btn_fwd  = (VixRect){lx, cy, 32.0f, 32.0f}; lx += 38.0f;
    L->btn_next = (VixRect){lx, cy, 32.0f, 32.0f}; lx += 38.0f;
    L->btn_stop = (VixRect){lx, cy, 32.0f, 32.0f}; lx += 42.0f;
    L->time_box = (VixRect){lx, cy, 138.0f, 32.0f};

    float bx = W - 18.0f;
    L->btn_fs = (VixRect){bx - 34.0f, cy, 34.0f, 32.0f}; bx -= 42.0f;
    if (W >= 860.0f) {
        L->btn_snap = (VixRect){bx - 56.0f, cy, 56.0f, 32.0f}; bx -= 64.0f;
    }
    if (W >= 740.0f) {
        L->btn_fx   = (VixRect){bx - 118.0f, cy, 118.0f, 32.0f}; bx -= 126.0f;
    }
    if (W >= 960.0f) {
        L->btn_sub  = (VixRect){bx - 56.0f, cy, 56.0f, 32.0f}; bx -= 62.0f;
        L->btn_aud  = (VixRect){bx - 56.0f, cy, 56.0f, 32.0f}; bx -= 62.0f;
    }
    if (W >= 800.0f) {
        L->btn_speed = (VixRect){bx - 58.0f, cy, 58.0f, 32.0f}; bx -= 66.0f;
    }
    L->vol_slider_hit = (VixRect){bx - 92.0f, cy, 92.0f, 32.0f};
    L->vol_slider_bar = (VixRect){bx - 88.0f, cy + 13.0f, 84.0f, 6.0f};
    bx -= 98.0f;
    L->btn_mute = (VixRect){bx - 32.0f, cy, 32.0f, 32.0f};

    /* Idle Centerpiece */
    float cw = fminf(490.0f, W - 40.0f);
    float ch = 330.0f;
    float icx = (W - cw) * 0.5f;
    float icy = (H - ch) * 0.5f - 10.0f;
    L->idle_card     = (VixRect){icx, icy, cw, ch};
    L->btn_idle_open = (VixRect){icx + cw * 0.5f - 186.0f, icy + 228.0f, 178.0f, 40.0f};
    L->btn_idle_url  = (VixRect){icx + cw * 0.5f + 8.0f,   icy + 228.0f, 178.0f, 40.0f};
    L->btn_idle_copy_cmd = (VixRect){icx + cw * 0.5f - 85.0f, icy + ch + 42.0f, 170.0f, 28.0f};

    /* Playlist Drawer */
    float dw = fminf(DRAWER_W, W * 0.45f);
    float dy = TOP_BAR_H + 6.0f;
    float dh = fmaxf(120.0f, H - TOP_BAR_H - BOT_DECK_H - 12.0f);
    L->drawer       = (VixRect){W - dw - 8.0f, dy, dw, dh};
    L->btn_pl_add   = (VixRect){L->drawer.x + 12.0f, dy + 42.0f, 108.0f, 28.0f};
    L->btn_pl_clear = (VixRect){L->drawer.x + 126.0f, dy + 42.0f, 96.0f, 28.0f};
    L->btn_pl_close = (VixRect){L->drawer.x + dw - 36.0f, dy + 8.0f, 26.0f, 26.0f};
    L->pl_list_area = (VixRect){L->drawer.x + 8.0f, dy + 78.0f, dw - 16.0f, fmaxf(40.0f, dh - 86.0f)};
}

static VixRect compute_browser_modal_rect(const VixPlayerUi *ui) {
    float mw = fminf(780.0f, (float)ui->win_w - 48.0f);
    float mh = fminf(500.0f, (float)ui->win_h - 56.0f);
    return (VixRect){((float)ui->win_w - mw) * 0.5f, ((float)ui->win_h - mh) * 0.5f, mw, mh};
}

static VixRect compute_url_modal_rect(const VixPlayerUi *ui) {
    float mw = fminf(580.0f, (float)ui->win_w - 48.0f);
    float mh = 216.0f;
    return (VixRect){((float)ui->win_w - mw) * 0.5f, ((float)ui->win_h - mh) * 0.5f, mw, mh};
}

/* ---------------- Public Init & Actions ---------------- */

void player_ui_init(VixPlayerUi *ui, int win_w, int win_h) {
    memset(ui, 0, sizeof(*ui));
    ui->win_w = win_w;
    ui->win_h = win_h;
    ui->hud_alpha = 1.0f;
    ui->selected_entry = -1;
    ui->last_clicked_entry = -1;

    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(ui->browser_dir, sizeof(ui->browser_dir), "%s", home);
    } else if (!getcwd(ui->browser_dir, sizeof(ui->browser_dir))) {
        snprintf(ui->browser_dir, sizeof(ui->browser_dir), "/");
    }
}

void player_ui_show_toast(VixPlayerUi *ui, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ui->toast_text, sizeof(ui->toast_text), fmt, ap);
    va_end(ap);
    ui->toast_until = ui->anim_time + 2.2;
}

void player_ui_open_file_modal(VixPlayerUi *ui, bool subtitle_only) {
    ui->modal = VIX_MODAL_FILE_BROWSER;
    ui->subtitle_pick_mode = subtitle_only;
    ui->last_activity_time = ui->anim_time;
    refresh_browser_dir(ui);
}

void player_ui_open_url_modal(VixPlayerUi *ui) {
    ui->modal = VIX_MODAL_OPEN_URL;
    ui->last_activity_time = ui->anim_time;
    SDL_StartTextInput();
}

static void activate_browser_entry(VixPlayerUi *ui, VixMpvEngine *eng, int idx) {
    if (idx < 0 || idx >= ui->dir_entry_count) return;
    const VixDirEntry *ent = &ui->dir_entries[idx];
    char full[VIX_PATH_MAX + 512];
    if (strcmp(ui->browser_dir, "/") == 0)
        snprintf(full, sizeof(full), "/%s", ent->name);
    else
        snprintf(full, sizeof(full), "%s/%s", ui->browser_dir, ent->name);

    if (ent->is_dir) {
        snprintf(ui->browser_dir, sizeof(ui->browser_dir), "%.*s", VIX_PATH_MAX - 1, full);
        refresh_browser_dir(ui);
        return;
    }

    if ((ui->subtitle_pick_mode || ent->is_subtitle) && !eng->idle_active) {
        mpv_engine_load_subtitle(eng, full);
        player_ui_show_toast(ui, "Субтитры подключены: %s", ent->name);
    } else {
        mpv_engine_load(eng, full, false);
        player_ui_show_toast(ui, "Открыт: %s", ent->name);
    }
    ui->modal = VIX_MODAL_NONE;
}

/* ---------------- Event Handling ---------------- */

void player_ui_handle_event(VixPlayerUi *ui, const SDL_Event *ev, VixMpvEngine *eng) {
    if (ev->type == SDL_WINDOWEVENT) {
        if (ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            ev->window.event == SDL_WINDOWEVENT_RESIZED) {
            ui->win_w = ev->window.data1;
            ui->win_h = ev->window.data2;
        }
        return;
    }

    if (ev->type == SDL_DROPFILE) {
        if (ev->drop.file) {
            if (is_subtitle_filename(ev->drop.file) && !eng->idle_active) {
                mpv_engine_load_subtitle(eng, ev->drop.file);
                player_ui_show_toast(ui, "Субтитры добавлены");
            } else {
                mpv_engine_load(eng, ev->drop.file, false);
                player_ui_show_toast(ui, "Загружено перетаскиванием");
            }
            SDL_free(ev->drop.file);
        }
        ui->last_activity_time = ui->anim_time;
        return;
    }

    if (ev->type == SDL_MOUSEMOTION) {
        ui->mouse_x = (float)ev->motion.x;
        ui->mouse_y = (float)ev->motion.y;
        ui->last_activity_time = ui->anim_time;

        HudLayout L;
        compute_hud_layout(ui, &L);
        if (ui->dragging_timeline && eng->duration > 0.0) {
            float t = (ui->mouse_x - L.timeline_bar.x) / fmaxf(1.0f, L.timeline_bar.w);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            mpv_engine_seek_absolute(eng, t * eng->duration);
        }
        if (ui->dragging_volume) {
            float t = (ui->mouse_x - L.vol_slider_bar.x) / fmaxf(1.0f, L.vol_slider_bar.w);
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            mpv_engine_set_volume(eng, t * 150.0);
        }
        return;
    }

    if (ev->type == SDL_MOUSEBUTTONUP && ev->button.button == SDL_BUTTON_LEFT) {
        ui->mouse_down = false;
        ui->dragging_timeline = false;
        ui->dragging_volume = false;
        return;
    }

    if (ev->type == SDL_MOUSEWHEEL) {
        ui->last_activity_time = ui->anim_time;
        if (ui->modal == VIX_MODAL_FILE_BROWSER) {
            ui->browser_scroll -= ev->wheel.y * 34.0f;
            float max_sc = fmaxf(0.0f, ui->dir_entry_count * 30.0f - 320.0f);
            if (ui->browser_scroll < 0.0f) ui->browser_scroll = 0.0f;
            if (ui->browser_scroll > max_sc) ui->browser_scroll = max_sc;
            return;
        }
        if (ui->show_playlist) {
            HudLayout L;
            compute_hud_layout(ui, &L);
            if (vix_rect_contains(L.drawer, ui->mouse_x, ui->mouse_y)) {
                ui->playlist_scroll -= ev->wheel.y * 30.0f;
                float max_sc = fmaxf(0.0f, eng->item_count * 34.0f - L.pl_list_area.h);
                if (ui->playlist_scroll < 0.0f) ui->playlist_scroll = 0.0f;
                if (ui->playlist_scroll > max_sc) ui->playlist_scroll = max_sc;
                return;
            }
        }
        mpv_engine_add_volume(eng, ev->wheel.y * 5.0);
        player_ui_show_toast(ui, "Громкость: %.0f%%", eng->volume);
        return;
    }

    /* -------- URL Modal Events -------- */
    if (ui->modal == VIX_MODAL_OPEN_URL) {
        if (ev->type == SDL_TEXTINPUT) {
            size_t cur = strlen(ui->url_input);
            size_t add = strlen(ev->text.text);
            if (cur + add + 1 < sizeof(ui->url_input)) {
                memcpy(ui->url_input + cur, ev->text.text, add + 1);
            }
            return;
        }
        if (ev->type == SDL_KEYDOWN) {
            SDL_Keycode sym = ev->key.keysym.sym;
            uint16_t mod = ev->key.keysym.mod;
            if (sym == SDLK_ESCAPE) {
                ui->modal = VIX_MODAL_NONE;
                SDL_StopTextInput();
            } else if (sym == SDLK_BACKSPACE) {
                utf8_pop_last_char(ui->url_input);
            } else if (sym == SDLK_v && (mod & KMOD_CTRL)) {
                if (SDL_HasClipboardText()) {
                    char *clip = SDL_GetClipboardText();
                    if (clip) {
                        snprintf(ui->url_input, sizeof(ui->url_input), "%s", clip);
                        SDL_free(clip);
                    }
                }
            } else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                if (ui->url_input[0]) {
                    mpv_engine_load(eng, ui->url_input, false);
                    player_ui_show_toast(ui, "Запуск потока: %s", ui->url_input);
                    ui->modal = VIX_MODAL_NONE;
                    SDL_StopTextInput();
                }
            }
            return;
        }
        if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
            float mx = (float)ev->button.x, my = (float)ev->button.y;
            VixRect M = compute_url_modal_rect(ui);
            VixRect btn_close = {M.x + M.w - 38.0f, M.y + 12.0f, 26.0f, 26.0f};
            VixRect btn_paste = {M.x + 20.0f, M.y + 154.0f, 118.0f, 36.0f};
            VixRect btn_clear = {M.x + 146.0f, M.y + 154.0f, 108.0f, 36.0f};
            VixRect btn_play  = {M.x + M.w - 170.0f, M.y + 154.0f, 150.0f, 36.0f};

            if (!vix_rect_contains(M, mx, my) || vix_rect_contains(btn_close, mx, my)) {
                ui->modal = VIX_MODAL_NONE;
                SDL_StopTextInput();
            } else if (vix_rect_contains(btn_paste, mx, my)) {
                if (SDL_HasClipboardText()) {
                    char *clip = SDL_GetClipboardText();
                    if (clip) {
                        snprintf(ui->url_input, sizeof(ui->url_input), "%s", clip);
                        SDL_free(clip);
                    }
                }
            } else if (vix_rect_contains(btn_clear, mx, my)) {
                ui->url_input[0] = '\0';
            } else if (vix_rect_contains(btn_play, mx, my) && ui->url_input[0]) {
                mpv_engine_load(eng, ui->url_input, false);
                player_ui_show_toast(ui, "Запуск URL потока");
                ui->modal = VIX_MODAL_NONE;
                SDL_StopTextInput();
            }
            return;
        }
        return;
    }

    /* -------- Built-in File Browser Modal Events -------- */
    if (ui->modal == VIX_MODAL_FILE_BROWSER) {
        if (ev->type == SDL_KEYDOWN) {
            SDL_Keycode sym = ev->key.keysym.sym;
            if (sym == SDLK_ESCAPE) {
                ui->modal = VIX_MODAL_NONE;
            } else if (sym == SDLK_BACKSPACE) {
                navigate_parent_dir(ui);
            } else if (sym == SDLK_UP && ui->dir_entry_count > 0) {
                if (ui->selected_entry > 0) ui->selected_entry--;
                else ui->selected_entry = 0;
            } else if (sym == SDLK_DOWN && ui->dir_entry_count > 0) {
                if (ui->selected_entry < ui->dir_entry_count - 1) ui->selected_entry++;
            } else if ((sym == SDLK_RETURN || sym == SDLK_KP_ENTER) && ui->selected_entry >= 0) {
                activate_browser_entry(ui, eng, ui->selected_entry);
            }
            return;
        }
        if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT) {
            float mx = (float)ev->button.x, my = (float)ev->button.y;
            VixRect M = compute_browser_modal_rect(ui);
            VixRect btn_close  = {M.x + M.w - 38.0f, M.y + 12.0f, 26.0f, 26.0f};
            VixRect btn_up     = {M.x + 170.0f, M.y + 48.0f, 92.0f, 28.0f};
            VixRect btn_all    = {M.x + M.w - 280.0f, M.y + 48.0f, 110.0f, 28.0f};
            VixRect btn_sysdlg = {M.x + M.w - 162.0f, M.y + 48.0f, 146.0f, 28.0f};
            VixRect btn_open   = {M.x + M.w - 156.0f, M.y + M.h - 44.0f, 140.0f, 32.0f};
            VixRect btn_cancel = {M.x + M.w - 266.0f, M.y + M.h - 44.0f, 102.0f, 32.0f};
            VixRect list_area  = {M.x + 170.0f, M.y + 84.0f, M.w - 186.0f, M.h - 138.0f};

            if (!vix_rect_contains(M, mx, my) || vix_rect_contains(btn_close, mx, my) ||
                vix_rect_contains(btn_cancel, mx, my)) {
                ui->modal = VIX_MODAL_NONE;
                return;
            }
            if (vix_rect_contains(btn_up, mx, my)) {
                navigate_parent_dir(ui);
                return;
            }
            if (vix_rect_contains(btn_all, mx, my)) {
                ui->show_all_files = !ui->show_all_files;
                refresh_browser_dir(ui);
                return;
            }
            if (vix_rect_contains(btn_sysdlg, mx, my)) {
                char chosen[VIX_PATH_MAX];
                if (try_native_file_dialog(chosen, sizeof(chosen))) {
                    mpv_engine_load(eng, chosen, false);
                    player_ui_show_toast(ui, "Файл открыт");
                    ui->modal = VIX_MODAL_NONE;
                } else {
                    player_ui_show_toast(ui, "zenity/kdialog не найден — используйте встроенный проводник");
                }
                return;
            }
            if (vix_rect_contains(btn_open, mx, my) && ui->selected_entry >= 0) {
                activate_browser_entry(ui, eng, ui->selected_entry);
                return;
            }

            /* Sidebar bookmarks */
            const char *home = getenv("HOME");
            const char *bm_sub[5] = { "", "/Videos", "/Downloads", "/Documents", NULL };
            for (int b = 0; b < 5; b++) {
                VixRect br = {M.x + 14.0f, M.y + 84.0f + b * 36.0f, 144.0f, 30.0f};
                if (vix_rect_contains(br, mx, my)) {
                    if (b == 4) {
                        snprintf(ui->browser_dir, sizeof(ui->browser_dir), "/");
                    } else if (home && *home) {
                        snprintf(ui->browser_dir, sizeof(ui->browser_dir), "%s%s", home, bm_sub[b]);
                    }
                    refresh_browser_dir(ui);
                    return;
                }
            }

            /* File list rows */
            if (vix_rect_contains(list_area, mx, my)) {
                int idx = (int)((my - list_area.y + ui->browser_scroll) / 30.0f);
                if (idx >= 0 && idx < ui->dir_entry_count) {
                    uint32_t now = SDL_GetTicks();
                    if (ui->last_clicked_entry == idx && (now - ui->last_click_ticks) < 400) {
                        activate_browser_entry(ui, eng, idx);
                    } else {
                        ui->selected_entry = idx;
                        ui->last_clicked_entry = idx;
                        ui->last_click_ticks = now;
                    }
                }
                return;
            }
            return;
        }
        return;
    }

    /* -------- Main Player Mouse Clicks -------- */
    if (ev->type == SDL_MOUSEBUTTONDOWN) {
        ui->last_activity_time = ui->anim_time;
        float mx = (float)ev->button.x;
        float my = (float)ev->button.y;
        HudLayout L;
        compute_hud_layout(ui, &L);

        if (ev->button.button == SDL_BUTTON_RIGHT) {
            if (vix_rect_contains(L.btn_speed, mx, my)) {
                mpv_engine_set_speed(eng, 1.0);
                player_ui_show_toast(ui, "Скорость сброшена: 1.0x");
                return;
            }
            if (vix_rect_contains(L.btn_sub, mx, my)) {
                player_ui_open_file_modal(ui, true);
                return;
            }
        }

        if (ev->button.button != SDL_BUTTON_LEFT) return;
        ui->mouse_down = true;

        /* Top bar clicks */
        if (vix_rect_contains(L.top_bar, mx, my)) {
            if (vix_rect_contains(L.btn_top_open, mx, my)) {
                player_ui_open_file_modal(ui, false);
            } else if (vix_rect_contains(L.btn_top_url, mx, my)) {
                player_ui_open_url_modal(ui);
            } else if (vix_rect_contains(L.btn_top_info, mx, my)) {
                ui->show_info_hud = !ui->show_info_hud;
            } else if (vix_rect_contains(L.btn_top_list, mx, my)) {
                ui->show_playlist = !ui->show_playlist;
            }
            return;
        }

        /* Playlist drawer clicks */
        if (ui->show_playlist && vix_rect_contains(L.drawer, mx, my)) {
            if (vix_rect_contains(L.btn_pl_close, mx, my)) {
                ui->show_playlist = false;
            } else if (vix_rect_contains(L.btn_pl_add, mx, my)) {
                player_ui_open_file_modal(ui, false);
            } else if (vix_rect_contains(L.btn_pl_clear, mx, my)) {
                mpv_engine_clear_playlist(eng);
                player_ui_show_toast(ui, "Плейлист очищен");
            } else if (vix_rect_contains(L.pl_list_area, mx, my)) {
                int row = (int)((my - L.pl_list_area.y + ui->playlist_scroll) / 34.0f);
                if (row >= 0 && row < eng->item_count) {
                    float del_x = L.pl_list_area.x + L.pl_list_area.w - 28.0f;
                    if (mx >= del_x) {
                        mpv_engine_remove_index(eng, row);
                    } else {
                        mpv_engine_play_index(eng, row);
                    }
                }
            }
            return;
        }

        /* Bottom control deck clicks */
        if (vix_rect_contains(L.bot_deck, mx, my)) {
            if (vix_rect_contains(L.timeline_hit, mx, my)) {
                ui->dragging_timeline = true;
                if (eng->duration > 0.0) {
                    float t = (mx - L.timeline_bar.x) / fmaxf(1.0f, L.timeline_bar.w);
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    mpv_engine_seek_absolute(eng, t * eng->duration);
                }
            } else if (vix_rect_contains(L.vol_slider_hit, mx, my)) {
                ui->dragging_volume = true;
                float t = (mx - L.vol_slider_bar.x) / fmaxf(1.0f, L.vol_slider_bar.w);
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                mpv_engine_set_volume(eng, t * 150.0);
            } else if (vix_rect_contains(L.btn_play, mx, my)) {
                if (eng->idle_active && eng->item_count == 0) {
                    player_ui_open_file_modal(ui, false);
                } else {
                    mpv_engine_toggle_pause(eng);
                }
            } else if (vix_rect_contains(L.btn_prev, mx, my)) {
                mpv_engine_prev(eng);
            } else if (vix_rect_contains(L.btn_next, mx, my)) {
                mpv_engine_next(eng);
            } else if (vix_rect_contains(L.btn_rew, mx, my)) {
                mpv_engine_seek_relative(eng, -10.0);
                player_ui_show_toast(ui, "Перемотка -10 сек");
            } else if (vix_rect_contains(L.btn_fwd, mx, my)) {
                mpv_engine_seek_relative(eng, 10.0);
                player_ui_show_toast(ui, "Перемотка +10 сек");
            } else if (vix_rect_contains(L.btn_stop, mx, my)) {
                mpv_engine_stop(eng);
                player_ui_show_toast(ui, "Остановлено");
            } else if (vix_rect_contains(L.btn_mute, mx, my)) {
                mpv_engine_toggle_mute(eng);
                player_ui_show_toast(ui, eng->muted ? "Звук выключен" : "Звук включён");
            } else if (L.btn_speed.w > 0 && vix_rect_contains(L.btn_speed, mx, my)) {
                mpv_engine_cycle_speed(eng, 1);
                player_ui_show_toast(ui, "Скорость: %.2gx", eng->speed);
            } else if (L.btn_aud.w > 0 && vix_rect_contains(L.btn_aud, mx, my)) {
                mpv_engine_cycle_audio(eng);
                player_ui_show_toast(ui, "Аудиодорожка переключена");
            } else if (L.btn_sub.w > 0 && vix_rect_contains(L.btn_sub, mx, my)) {
                mpv_engine_cycle_sub(eng);
                player_ui_show_toast(ui, "Субтитры переключены (ПКМ — загрузить файл)");
            } else if (L.btn_fx.w > 0 && vix_rect_contains(L.btn_fx, mx, my)) {
                mpv_engine_cycle_fx(eng);
                player_ui_show_toast(ui, "VIXGlitch FX: %s", mpv_engine_fx_name(eng->active_fx));
            } else if (L.btn_snap.w > 0 && vix_rect_contains(L.btn_snap, mx, my)) {
                char msg[256];
                mpv_engine_screenshot(eng, msg, sizeof(msg));
                player_ui_show_toast(ui, "%s", msg);
            } else if (vix_rect_contains(L.btn_fs, mx, my)) {
                ui->request_toggle_fullscreen = true;
            }
            return;
        }

        /* Idle centerpiece buttons */
        if (eng->idle_active) {
            if (vix_rect_contains(L.btn_idle_open, mx, my)) {
                player_ui_open_file_modal(ui, false);
                return;
            }
            if (vix_rect_contains(L.btn_idle_url, mx, my)) {
                player_ui_open_url_modal(ui);
                return;
            }
            if (!eng->libmpv_available && vix_rect_contains(L.btn_idle_copy_cmd, mx, my)) {
                SDL_SetClipboardText("sudo pacman -S mpv");
                player_ui_show_toast(ui, "Скопировано: sudo pacman -S mpv");
                return;
            }
        }

        /* Video viewport click / double-click */
        if (ev->button.clicks >= 2) {
            ui->request_toggle_fullscreen = true;
        } else if (!eng->idle_active) {
            mpv_engine_toggle_pause(eng);
        }
        return;
    }

    /* -------- Main Player Keyboard Shortcuts (mpv compatible!) -------- */
    if (ev->type == SDL_KEYDOWN) {
        ui->last_activity_time = ui->anim_time;
        SDL_Keycode sym = ev->key.keysym.sym;
        uint16_t mod = ev->key.keysym.mod;

        if ((sym == SDLK_q && (mod & KMOD_CTRL)) || sym == SDLK_q) {
            ui->request_quit = true;
        } else if (sym == SDLK_o && (mod & KMOD_CTRL)) {
            player_ui_open_file_modal(ui, false);
        } else if (sym == SDLK_o) {
            player_ui_open_file_modal(ui, false);
        } else if (sym == SDLK_u && (mod & KMOD_CTRL)) {
            player_ui_open_url_modal(ui);
        } else if (sym == SDLK_SPACE || sym == SDLK_k) {
            mpv_engine_toggle_pause(eng);
            player_ui_show_toast(ui, eng->paused ? "Пауза" : "Воспроизведение");
        } else if (sym == SDLK_f || sym == SDLK_F11) {
            ui->request_toggle_fullscreen = true;
        } else if (sym == SDLK_ESCAPE) {
            if (ui->show_playlist) ui->show_playlist = false;
            else if (ui->show_info_hud) ui->show_info_hud = false;
            else if (ui->is_fullscreen) ui->request_toggle_fullscreen = true;
        } else if (sym == SDLK_LEFT) {
            double d = (mod & KMOD_SHIFT) ? -1.0 : ((mod & KMOD_CTRL) ? -30.0 : -5.0);
            mpv_engine_seek_relative(eng, d);
            player_ui_show_toast(ui, "Перемотка %.0f сек", d);
        } else if (sym == SDLK_RIGHT) {
            double d = (mod & KMOD_SHIFT) ? 1.0 : ((mod & KMOD_CTRL) ? 30.0 : 5.0);
            mpv_engine_seek_relative(eng, d);
            player_ui_show_toast(ui, "Перемотка +%.0f сек", d);
        } else if (sym == SDLK_UP || sym == SDLK_0) {
            mpv_engine_add_volume(eng, 5.0);
            player_ui_show_toast(ui, "Громкость: %.0f%%", eng->volume);
        } else if (sym == SDLK_DOWN || sym == SDLK_9) {
            mpv_engine_add_volume(eng, -5.0);
            player_ui_show_toast(ui, "Громкость: %.0f%%", eng->volume);
        } else if (sym == SDLK_m) {
            mpv_engine_toggle_mute(eng);
            player_ui_show_toast(ui, eng->muted ? "Без звука (Mute)" : "Звук включён");
        } else if (sym == SDLK_LEFTBRACKET) {
            mpv_engine_set_speed(eng, eng->speed - 0.1);
            player_ui_show_toast(ui, "Скорость: %.2fx", eng->speed);
        } else if (sym == SDLK_RIGHTBRACKET) {
            mpv_engine_set_speed(eng, eng->speed + 0.1);
            player_ui_show_toast(ui, "Скорость: %.2fx", eng->speed);
        } else if (sym == SDLK_BACKSPACE) {
            mpv_engine_set_speed(eng, 1.0);
            player_ui_show_toast(ui, "Скорость: 1.0x");
        } else if (sym == SDLK_PERIOD) {
            mpv_engine_frame_step(eng, false);
            player_ui_show_toast(ui, "Кадр вперёд");
        } else if (sym == SDLK_COMMA) {
            mpv_engine_frame_step(eng, true);
            player_ui_show_toast(ui, "Кадр назад");
        } else if (sym == SDLK_j) {
            mpv_engine_cycle_sub(eng);
            player_ui_show_toast(ui, "Субтитры: %s", eng->sid_str);
        } else if (sym == SDLK_v) {
            mpv_engine_toggle_sub_visibility(eng);
            player_ui_show_toast(ui, eng->sub_visible ? "Субтитры показаны" : "Субтитры скрыты");
        } else if (sym == SDLK_a || sym == SDLK_3) {
            if (sym == SDLK_a) {
                mpv_engine_cycle_audio(eng);
                player_ui_show_toast(ui, "Аудиодорожка переключена");
            } else {
                mpv_engine_adjust_eq(eng, "brightness", -2);
                player_ui_show_toast(ui, "Яркость -2");
            }
        } else if (sym == SDLK_4) {
            mpv_engine_adjust_eq(eng, "brightness", 2);
            player_ui_show_toast(ui, "Яркость +2");
        } else if (sym == SDLK_1) {
            mpv_engine_adjust_eq(eng, "contrast", -2);
            player_ui_show_toast(ui, "Контраст -2");
        } else if (sym == SDLK_2) {
            mpv_engine_adjust_eq(eng, "contrast", 2);
            player_ui_show_toast(ui, "Контраст +2");
        } else if (sym == SDLK_7) {
            mpv_engine_adjust_eq(eng, "saturation", -2);
            player_ui_show_toast(ui, "Насыщенность -2");
        } else if (sym == SDLK_8) {
            mpv_engine_adjust_eq(eng, "saturation", 2);
            player_ui_show_toast(ui, "Насыщенность +2");
        } else if (sym == SDLK_g) {
            mpv_engine_cycle_fx(eng);
            player_ui_show_toast(ui, "VIXGlitch FX: %s", mpv_engine_fx_name(eng->active_fx));
        } else if (sym == SDLK_s) {
            char msg[256];
            mpv_engine_screenshot(eng, msg, sizeof(msg));
            player_ui_show_toast(ui, "%s", msg);
        } else if (sym == SDLK_l) {
            char msg[256];
            mpv_engine_ab_loop(eng, msg, sizeof(msg));
            player_ui_show_toast(ui, "%s", msg);
        } else if (sym == SDLK_p) {
            ui->show_playlist = !ui->show_playlist;
        } else if (sym == SDLK_i || sym == SDLK_TAB) {
            ui->show_info_hud = !ui->show_info_hud;
        } else if (sym == SDLK_PAGEUP) {
            mpv_engine_prev(eng);
        } else if (sym == SDLK_PAGEDOWN) {
            mpv_engine_next(eng);
        }
    }
}

/* ---------------- Update & Smooth Animations ---------------- */

bool player_ui_update(VixPlayerUi *ui, double dt, const VixMpvEngine *eng) {
    ui->anim_time += dt;

    HudLayout L;
    compute_hud_layout(ui, &L);
    bool hovering_hud = vix_rect_contains(L.top_bar, ui->mouse_x, ui->mouse_y) ||
                        vix_rect_contains(L.bot_deck, ui->mouse_x, ui->mouse_y) ||
                        ui->dragging_timeline || ui->dragging_volume ||
                        ui->show_playlist || (ui->modal != VIX_MODAL_NONE);

    bool force_show = eng->idle_active || eng->paused || hovering_hud;
    double idle_for = ui->anim_time - ui->last_activity_time;
    float target_alpha = (force_show || idle_for < 2.6) ? 1.0f : 0.0f;

    float prev_alpha = ui->hud_alpha;
    float speed = (target_alpha > ui->hud_alpha) ? 9.0f : 3.5f;
    ui->hud_alpha += (target_alpha - ui->hud_alpha) * fminf(1.0f, (float)dt * speed);
    if (fabsf(ui->hud_alpha - target_alpha) < 0.005f) ui->hud_alpha = target_alpha;

    bool toast_active = ui->anim_time < ui->toast_until;
    return (fabsf(ui->hud_alpha - prev_alpha) > 0.001f) || eng->idle_active || toast_active;
}

/* ---------------- UI Rendering Helpers ---------------- */

static void draw_neon_button(VixUiRenderer *r, VixRect box, const char *label,
                             bool hovered, bool primary, bool active_state, float alpha) {
    VixColor fill   = primary ? (hovered ? vix_rgba(126, 38, 218, 0.96f) : vix_rgba(92, 26, 172, 0.90f))
                              : (active_state ? VIX_COL_BTN_ACTIVE : (hovered ? VIX_COL_BTN_HOVER : VIX_COL_BTN_BG));
    VixColor border = (primary || hovered || active_state) ? VIX_COL_NEON_BRIGHT : VIX_COL_BORDER_DIM;
    VixColor glow   = (primary || hovered || active_state)
                      ? vix_rgba(184, 77, 255, primary ? 0.55f : 0.42f)
                      : (VixColor){0, 0, 0, 0};
    float gr        = (primary || hovered || active_state) ? 10.0f : 0.0f;

    ui_draw_box(r, box, vix_alpha(fill, alpha), vix_alpha(border, alpha),
                7.0f, 1.2f, vix_alpha(glow, alpha), gr);
    if (label && *label) {
        VixColor tc = (primary || hovered || active_state) ? VIX_COL_TEXT_MAIN : VIX_COL_TEXT_SUB;
        ui_draw_text_centered(r, VIX_FONT_SMALL, box, label, vix_alpha(tc, alpha));
    }
}

static void draw_icon_button(VixUiRenderer *r, VixRect box, VixIconType icon,
                             bool hovered, bool primary, float alpha) {
    draw_neon_button(r, box, NULL, hovered, primary, false, alpha);
    float pad = primary ? 10.0f : 8.5f;
    VixRect ir = {box.x + pad, box.y + pad, box.w - pad * 2.0f, box.h - pad * 2.0f};
    VixColor ic = (primary || hovered) ? VIX_COL_TEXT_MAIN : VIX_COL_NEON_BRIGHT;
    ui_draw_icon(r, icon, ir, vix_alpha(ic, alpha));
}

/* ---------------- Main Render Pass ---------------- */

void player_ui_render(VixPlayerUi *ui, VixUiRenderer *r, VixMpvEngine *eng) {
    VixColor zero = {0, 0, 0, 0};
    HudLayout L;
    compute_hud_layout(ui, &L);
    float mx = ui->mouse_x, my = ui->mouse_y;

    /* 1. Idle State Showcase on Pure #000000 Black */
    if (eng->idle_active) {
        float pulse = 0.5f + 0.5f * sinf((float)ui->anim_time * 2.2f);
        VixRect C = L.idle_card;

        /* Subtle dark obsidian-lilac glass card over #000000 */
        ui_draw_box(r, C,
                    vix_rgba(9, 4, 18, 0.86f),
                    vix_rgba(168, 85, 247, 0.48f + 0.18f * pulse),
                    16.0f, 1.3f,
                    vix_rgba(168, 85, 247, 0.26f + 0.14f * pulse),
                    24.0f);

        /* Cyber-glitch corner accents */
        VixColor accent = VIX_COL_NEON_PINK;
        ui_draw_box(r, (VixRect){C.x + 12, C.y + 12, 22, 2}, accent, zero, 1, 0, zero, 0);
        ui_draw_box(r, (VixRect){C.x + 12, C.y + 12, 2, 22}, accent, zero, 1, 0, zero, 0);
        ui_draw_box(r, (VixRect){C.x + C.w - 34, C.y + 12, 22, 2}, accent, zero, 1, 0, zero, 0);
        ui_draw_box(r, (VixRect){C.x + C.w - 14, C.y + 12, 2, 22}, accent, zero, 1, 0, zero, 0);

        /* VIXGlitcher Attached Icon with Neon Lilac Aura */
        VixRect logo_box = {C.x + (C.w - 96.0f) * 0.5f, C.y + 24.0f, 96.0f, 96.0f};
        ui_draw_box(r, (VixRect){logo_box.x - 3, logo_box.y - 3, 102.0f, 102.0f},
                    vix_rgba(20, 8, 38, 1.0f),
                    VIX_COL_NEON_LILAC, 20.0f, 1.8f,
                    vix_rgba(192, 92, 255, 0.52f + 0.20f * pulse), 18.0f);
        ui_draw_logo(r, logo_box, 18.0f, 1.0f);

        /* Title & Subtitle */
        ui_draw_text_centered(r, VIX_FONT_TITLE,
                              (VixRect){C.x, C.y + 132.0f, C.w, 32.0f},
                              "VIXGlitcher", VIX_COL_TEXT_MAIN);
        ui_draw_text_centered(r, VIX_FONT_SMALL,
                              (VixRect){C.x, C.y + 165.0f, C.w, 20.0f},
                              "MPV ENGINE  •  DARK NEON LILAC  •  ARCH LINUX",
                              VIX_COL_NEON_BRIGHT);
        ui_draw_text_centered(r, VIX_FONT_SMALL,
                              (VixRect){C.x, C.y + 190.0f, C.w, 20.0f},
                              "Перетащите видео в окно или выберите источник",
                              VIX_COL_TEXT_MUTED);

        /* Action Buttons */
        bool hov_open = (ui->modal == VIX_MODAL_NONE) && vix_rect_contains(L.btn_idle_open, mx, my);
        bool hov_url  = (ui->modal == VIX_MODAL_NONE) && vix_rect_contains(L.btn_idle_url, mx, my);
        draw_neon_button(r, L.btn_idle_open, "ОТКРЫТЬ ФАЙЛ", hov_open, true, false, 1.0f);
        draw_neon_button(r, L.btn_idle_url,  "ОТКРЫТЬ URL",  hov_url,  false, false, 1.0f);

        ui_draw_text_centered(r, VIX_FONT_SMALL,
                              (VixRect){C.x, C.y + 284.0f, C.w, 20.0f},
                              "Space Пауза  •  F Полный экран  •  G Glitch FX  •  P Плейлист",
                              VIX_COL_TEXT_MUTED);

        /* Helpful Arch Linux notice if libmpv is missing on host */
        if (!eng->libmpv_available) {
            VixRect warn = {C.x, C.y + C.h + 10.0f, C.w, 66.0f};
            ui_draw_box(r, warn, vix_rgba(28, 8, 44, 0.92f),
                        VIX_COL_NEON_PINK, 10.0f, 1.2f,
                        vix_rgba(232, 121, 249, 0.25f), 10.0f);
            ui_draw_text_centered(r, VIX_FONT_SMALL,
                                  (VixRect){warn.x, warn.y + 8.0f, warn.w, 20.0f},
                                  eng->status_error, VIX_COL_TEXT_MAIN);
            bool hov_cp = vix_rect_contains(L.btn_idle_copy_cmd, mx, my);
            draw_neon_button(r, L.btn_idle_copy_cmd, "СКОПИРОВАТЬ КОМАНДУ", hov_cp, false, false, 1.0f);
        }
    }

    /* 2. Auto-Hiding Top & Bottom Dark Neon Lilac Bars */
    float a = ui->hud_alpha;
    if (a > 0.01f) {
        /* ---- Top Bar ---- */
        ui_draw_box(r, L.top_bar,
                    vix_alpha(VIX_COL_BAR_GLASS, a),
                    zero, 0.0f, 0.0f, zero, 0.0f);
        ui_draw_box(r, (VixRect){0, TOP_BAR_H - 1.2f, (float)ui->win_w, 1.2f},
                    vix_alpha(VIX_COL_NEON_LILAC, a * 0.65f),
                    zero, 0.0f, 0.0f,
                    vix_alpha(VIX_COL_GLOW_LILAC, a * 0.45f), 8.0f);

        ui_draw_logo(r, (VixRect){14.0f, 9.0f, 28.0f, 28.0f}, 6.0f, a);

        VixRect badge = {50.0f, 10.0f, 118.0f, 26.0f};
        ui_draw_box(r, badge, vix_alpha(vix_rgba(44, 14, 82, 0.85f), a),
                    vix_alpha(VIX_COL_NEON_LILAC, a * 0.7f), 6.0f, 1.0f, zero, 0.0f);
        ui_draw_text_centered(r, VIX_FONT_SMALL, badge, "VIXGLITCHER",
                              vix_alpha(VIX_COL_NEON_BRIGHT, a));

        const char *title = (!eng->idle_active && eng->media_title[0])
                            ? eng->media_title
                            : "Готов к воспроизведению  •  mpv neon-lilac";
        float title_max_w = L.btn_top_open.x - 190.0f;
        if (title_max_w > 40.0f) {
            ui_draw_text_ellipsis(r, VIX_FONT_BODY, 180.0f, 14.0f, title_max_w,
                                  title, vix_alpha(eng->idle_active ? VIX_COL_TEXT_MUTED : VIX_COL_TEXT_MAIN, a));
        }

        char list_lbl[48];
        snprintf(list_lbl, sizeof(list_lbl), "ПЛЕЙЛИСТ [%d]", eng->item_count);
        bool can_hover = (ui->modal == VIX_MODAL_NONE);
        draw_neon_button(r, L.btn_top_open, "ФАЙЛ",    can_hover && vix_rect_contains(L.btn_top_open, mx, my), false, false, a);
        draw_neon_button(r, L.btn_top_url,  "URL",     can_hover && vix_rect_contains(L.btn_top_url, mx, my),  false, false, a);
        draw_neon_button(r, L.btn_top_info, "ИНФО",    can_hover && vix_rect_contains(L.btn_top_info, mx, my), false, ui->show_info_hud, a);
        draw_neon_button(r, L.btn_top_list, list_lbl,  can_hover && vix_rect_contains(L.btn_top_list, mx, my), false, ui->show_playlist, a);

        /* ---- Bottom Control Deck ---- */
        ui_draw_box(r, L.bot_deck,
                    vix_alpha(VIX_COL_BAR_GLASS, a),
                    zero, 0.0f, 0.0f, zero, 0.0f);
        ui_draw_box(r, (VixRect){0, L.bot_deck.y, (float)ui->win_w, 1.3f},
                    vix_alpha(VIX_COL_NEON_LILAC, a * 0.78f),
                    zero, 0.0f, 0.0f,
                    vix_alpha(VIX_COL_GLOW_LILAC, a * 0.55f), 10.0f);

        /* Timeline Scrubber */
        bool hov_tl = can_hover && (vix_rect_contains(L.timeline_hit, mx, my) || ui->dragging_timeline);
        VixRect tbar = L.timeline_bar;
        if (hov_tl) { tbar.y -= 1.0f; tbar.h += 2.0f; }

        ui_draw_box(r, tbar, vix_alpha(VIX_COL_TRACK_BG, a),
                    vix_alpha(VIX_COL_BORDER_DIM, a * 0.6f), 4.0f, 1.0f, zero, 0.0f);

        float progress = (eng->duration > 0.0) ? (float)(eng->time_pos / eng->duration) : 0.0f;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;

        if (eng->duration > 0.0 && eng->cache_time > 0.0) {
            float cp = (float)((eng->time_pos + eng->cache_time) / eng->duration);
            if (cp > 1.0f) cp = 1.0f;
            ui_draw_box(r, (VixRect){tbar.x, tbar.y, tbar.w * cp, tbar.h},
                        vix_alpha(VIX_COL_TRACK_CACHE, a), zero, 4.0f, 0, zero, 0);
        }

        float fill_w = tbar.w * progress;
        if (fill_w > 2.0f) {
            ui_draw_box(r, (VixRect){tbar.x, tbar.y, fill_w, tbar.h},
                        vix_alpha(VIX_COL_NEON_LILAC, a),
                        zero, 4.0f, 0.0f,
                        vix_alpha(VIX_COL_GLOW_LILAC, a * 0.8f), 8.0f);
        }

        /* A-B loop markers */
        if (eng->duration > 0.0 && eng->ab_loop_a >= 0.0) {
            float ax = tbar.x + tbar.w * clampf((float)(eng->ab_loop_a / eng->duration), 0.0f, 1.0f);
            ui_draw_box(r, (VixRect){ax - 1.5f, tbar.y - 4.0f, 3.0f, tbar.h + 8.0f},
                        vix_alpha(VIX_COL_NEON_PINK, a), zero, 1.0f, 0, zero, 0);
        }
        if (eng->duration > 0.0 && eng->ab_loop_b >= 0.0) {
            float bx = tbar.x + tbar.w * clampf((float)(eng->ab_loop_b / eng->duration), 0.0f, 1.0f);
            ui_draw_box(r, (VixRect){bx - 1.5f, tbar.y - 4.0f, 3.0f, tbar.h + 8.0f},
                        vix_alpha(VIX_COL_NEON_PINK, a), zero, 1.0f, 0, zero, 0);
        }

        /* Glowing Scrubber Thumb */
        float knob_r = hov_tl ? 7.5f : 5.5f;
        float knob_x = tbar.x + fill_w;
        float knob_y = tbar.y + tbar.h * 0.5f;
        ui_draw_box(r, (VixRect){knob_x - knob_r, knob_y - knob_r, knob_r * 2.0f, knob_r * 2.0f},
                    vix_alpha(VIX_COL_NEON_BRIGHT, a),
                    vix_alpha(VIX_COL_TEXT_MAIN, a), knob_r, 1.5f,
                    vix_alpha(VIX_COL_NEON_LILAC, a * 0.9f), 10.0f);

        /* Hover Timestamp Tooltip */
        if (hov_tl && eng->duration > 0.0) {
            float ht = clampf((mx - tbar.x) / fmaxf(1.0f, tbar.w), 0.0f, 1.0f);
            char hbuf[32];
            format_timecode(ht * eng->duration, hbuf, sizeof(hbuf));
            float pw = ui_text_width(r, VIX_FONT_SMALL, hbuf) + 18.0f;
            float px = clampf(mx - pw * 0.5f, 12.0f, (float)ui->win_w - pw - 12.0f);
            VixRect pill = {px, tbar.y - 30.0f, pw, 22.0f};
            ui_draw_box(r, pill, vix_rgba(18, 6, 36, 0.96f), VIX_COL_NEON_LILAC, 6.0f, 1.0f, zero, 0);
            ui_draw_text_centered(r, VIX_FONT_SMALL, pill, hbuf, VIX_COL_TEXT_MAIN);
        }

        /* Transport Buttons */
        draw_icon_button(r, L.btn_prev, VIX_ICON_PREV,    can_hover && vix_rect_contains(L.btn_prev, mx, my), false, a);
        draw_icon_button(r, L.btn_rew,  VIX_ICON_REWIND,  can_hover && vix_rect_contains(L.btn_rew, mx, my),  false, a);
        VixIconType play_ic = (!eng->idle_active && !eng->paused) ? VIX_ICON_PAUSE : VIX_ICON_PLAY;
        draw_icon_button(r, L.btn_play, play_ic,          can_hover && vix_rect_contains(L.btn_play, mx, my), true,  a);
        draw_icon_button(r, L.btn_fwd,  VIX_ICON_FORWARD, can_hover && vix_rect_contains(L.btn_fwd, mx, my),  false, a);
        draw_icon_button(r, L.btn_next, VIX_ICON_NEXT,    can_hover && vix_rect_contains(L.btn_next, mx, my), false, a);
        draw_icon_button(r, L.btn_stop, VIX_ICON_STOP,    can_hover && vix_rect_contains(L.btn_stop, mx, my), false, a);

        char t_cur[32], t_dur[32], t_full[80];
        format_timecode(eng->time_pos, t_cur, sizeof(t_cur));
        format_timecode(eng->duration, t_dur, sizeof(t_dur));
        snprintf(t_full, sizeof(t_full), "%s / %s", t_cur, t_dur);
        ui_draw_text(r, VIX_FONT_SMALL, L.time_box.x, L.time_box.y + 9.0f,
                     t_full, vix_alpha(VIX_COL_TEXT_SUB, a));

        /* Volume & Feature Controls */
        VixIconType vol_ic = (eng->muted || eng->volume < 1.0)
                             ? VIX_ICON_VOLUME_MUTE
                             : (eng->volume < 65.0 ? VIX_ICON_VOLUME_LOW : VIX_ICON_VOLUME_HIGH);
        draw_icon_button(r, L.btn_mute, vol_ic, can_hover && vix_rect_contains(L.btn_mute, mx, my), false, a);

        VixRect vbar = L.vol_slider_bar;
        ui_draw_box(r, vbar, vix_alpha(VIX_COL_TRACK_BG, a),
                    vix_alpha(VIX_COL_BORDER_DIM, a * 0.5f), 3.0f, 1.0f, zero, 0);
        float vfrac = eng->muted ? 0.0f : clampf((float)(eng->volume / 150.0), 0.0f, 1.0f);
        if (vfrac > 0.01f) {
            ui_draw_box(r, (VixRect){vbar.x, vbar.y, vbar.w * vfrac, vbar.h},
                        vix_alpha(VIX_COL_NEON_LILAC, a), zero, 3.0f, 0,
                        vix_alpha(VIX_COL_GLOW_LILAC, a * 0.6f), 6.0f);
        }
        float vx = vbar.x + vbar.w * vfrac;
        float vy = vbar.y + vbar.h * 0.5f;
        ui_draw_box(r, (VixRect){vx - 5.0f, vy - 5.0f, 10.0f, 10.0f},
                    vix_alpha(VIX_COL_NEON_BRIGHT, a), zero, 5.0f, 0, zero, 0);

        if (L.btn_speed.w > 0) {
            char sp_lbl[24];
            snprintf(sp_lbl, sizeof(sp_lbl), "%.2gx", eng->speed);
            draw_neon_button(r, L.btn_speed, sp_lbl,
                             can_hover && vix_rect_contains(L.btn_speed, mx, my),
                             false, fabs(eng->speed - 1.0) > 0.02, a);
        }
        if (L.btn_aud.w > 0) {
            draw_neon_button(r, L.btn_aud, "AUD",
                             can_hover && vix_rect_contains(L.btn_aud, mx, my), false, false, a);
        }
        if (L.btn_sub.w > 0) {
            draw_neon_button(r, L.btn_sub, "SUB",
                             can_hover && vix_rect_contains(L.btn_sub, mx, my),
                             false, eng->sub_visible && strcmp(eng->sid_str, "no") != 0 && strcmp(eng->sid_str, "auto") != 0, a);
        }
        if (L.btn_fx.w > 0) {
            char fx_lbl[40];
            snprintf(fx_lbl, sizeof(fx_lbl), "FX: %s", mpv_engine_fx_name(eng->active_fx));
            draw_neon_button(r, L.btn_fx, fx_lbl,
                             can_hover && vix_rect_contains(L.btn_fx, mx, my),
                             false, eng->active_fx != VIX_FX_OFF, a);
        }
        if (L.btn_snap.w > 0) {
            draw_neon_button(r, L.btn_snap, "SNAP",
                             can_hover && vix_rect_contains(L.btn_snap, mx, my), false, false, a);
        }
        draw_icon_button(r, L.btn_fs,
                         ui->is_fullscreen ? VIX_ICON_EXIT_FULLSCREEN : VIX_ICON_FULLSCREEN,
                         can_hover && vix_rect_contains(L.btn_fs, mx, my), false, a);
    }

    /* 3. Technical mpv Media Info HUD (I / ИНФО) */
    if (ui->show_info_hud) {
        VixRect H = {16.0f, TOP_BAR_H + 12.0f, 360.0f, 188.0f};
        ui_draw_box(r, H, vix_rgba(10, 4, 20, 0.92f), VIX_COL_NEON_LILAC,
                    10.0f, 1.2f, vix_rgba(184, 77, 255, 0.30f), 14.0f);
        ui_draw_text(r, VIX_FONT_BODY, H.x + 14.0f, H.y + 12.0f,
                     "VIXGLITCHER // СТАТУС MPV", VIX_COL_NEON_BRIGHT);

        char l1[512], l2[512], l3[512], l4[512], l5[512], l6[512];
        snprintf(l1, sizeof(l1), "Движок:    %s (API %lu.%lu)",
                 eng->libmpv_available ? eng->libmpv_soname : "не найден",
                 (eng->api_version >> 16) & 0xFFFF, eng->api_version & 0xFFFF);
        snprintf(l2, sizeof(l2), "Видео:     %s  %lldx%lld @ %.2f fps",
                 eng->video_codec[0] ? eng->video_codec : "—",
                 (long long)eng->video_w, (long long)eng->video_h, eng->fps);
        snprintf(l3, sizeof(l3), "Декодер:   hwdec=%s", eng->hwdec_current);
        snprintf(l4, sizeof(l4), "Аудио:     %s (aid=%s, Громк. %.0f%%)",
                 eng->audio_codec[0] ? eng->audio_codec : "—", eng->aid_str, eng->volume);
        snprintf(l5, sizeof(l5), "Субтитры:  sid=%s (%s)",
                 eng->sid_str, eng->sub_visible ? "вкл" : "скрыты");
        snprintf(l6, sizeof(l6), "Glitch FX: %s  |  Скорость: %.2fx",
                 mpv_engine_fx_name(eng->active_fx), eng->speed);

        const char *lines[6] = {l1, l2, l3, l4, l5, l6};
        for (int i = 0; i < 6; i++) {
            ui_draw_text_ellipsis(r, VIX_FONT_SMALL, H.x + 14.0f, H.y + 42.0f + i * 22.0f,
                                  H.w - 28.0f, lines[i], VIX_COL_TEXT_SUB);
        }
    }

    /* 4. Playlist Side Drawer (P / ПЛЕЙЛИСТ) */
    if (ui->show_playlist) {
        ui_draw_box(r, L.drawer, VIX_COL_MODAL_BG, VIX_COL_NEON_LILAC,
                    12.0f, 1.2f, vix_rgba(184, 77, 255, 0.30f), 16.0f);
        ui_draw_text(r, VIX_FONT_BODY, L.drawer.x + 14.0f, L.drawer.y + 12.0f,
                     "ПЛЕЙЛИСТ MPV", VIX_COL_NEON_BRIGHT);
        draw_icon_button(r, L.btn_pl_close, VIX_ICON_CLOSE,
                         vix_rect_contains(L.btn_pl_close, mx, my), false, 1.0f);
        draw_neon_button(r, L.btn_pl_add, "+ ДОБАВИТЬ",
                         vix_rect_contains(L.btn_pl_add, mx, my), true, false, 1.0f);
        draw_neon_button(r, L.btn_pl_clear, "ОЧИСТИТЬ",
                         vix_rect_contains(L.btn_pl_clear, mx, my), false, false, 1.0f);

        ui_push_clip(r, L.pl_list_area);
        if (eng->item_count == 0) {
            ui_draw_text_centered(r, VIX_FONT_SMALL, L.pl_list_area,
                                  "Плейлист пуст", VIX_COL_TEXT_MUTED);
        } else {
            for (int i = 0; i < eng->item_count; i++) {
                float ry = L.pl_list_area.y + i * 34.0f - ui->playlist_scroll;
                if (ry + 32.0f < L.pl_list_area.y || ry > L.pl_list_area.y + L.pl_list_area.h) continue;

                VixRect row = {L.pl_list_area.x, ry, L.pl_list_area.w, 30.0f};
                bool active = (i == (int)eng->playlist_pos);
                bool hov = (ui->modal == VIX_MODAL_NONE) && vix_rect_contains(row, mx, my);

                ui_draw_box(r, row,
                            active ? vix_rgba(86, 26, 152, 0.85f) : (hov ? VIX_COL_BTN_HOVER : vix_rgba(20, 8, 38, 0.65f)),
                            active ? VIX_COL_NEON_BRIGHT : zero,
                            6.0f, active ? 1.0f : 0.0f, zero, 0);
                char num[16];
                snprintf(num, sizeof(num), "%02d.", i + 1);
                ui_draw_text(r, VIX_FONT_SMALL, row.x + 8.0f, row.y + 8.0f, num, VIX_COL_NEON_BRIGHT);
                ui_draw_text_ellipsis(r, VIX_FONT_SMALL, row.x + 36.0f, row.y + 8.0f,
                                      row.w - 66.0f, eng->items[i].title, VIX_COL_TEXT_MAIN);
                ui_draw_text(r, VIX_FONT_SMALL, row.x + row.w - 20.0f, row.y + 8.0f,
                             "×", hov ? VIX_COL_NEON_PINK : VIX_COL_TEXT_MUTED);
            }
        }
        ui_pop_clip(r);
    }

    /* 5. Built-in Dark Neon Lilac File Browser Modal */
    if (ui->modal == VIX_MODAL_FILE_BROWSER) {
        ui_draw_box(r, (VixRect){0, 0, (float)ui->win_w, (float)ui->win_h},
                    vix_rgba(0, 0, 0, 0.68f), zero, 0, 0, zero, 0);

        VixRect M = compute_browser_modal_rect(ui);
        ui_draw_box(r, M, VIX_COL_MODAL_BG, VIX_COL_NEON_LILAC,
                    14.0f, 1.5f, vix_rgba(184, 77, 255, 0.45f), 22.0f);

        ui_draw_text(r, VIX_FONT_BODY, M.x + 18.0f, M.y + 15.0f,
                     ui->subtitle_pick_mode ? "ЗАГРУЗИТЬ СУБТИТРЫ (.SRT / .ASS)" : "ОТКРЫТЬ МЕДИАФАЙЛ  •  VIXGLITCHER",
                     VIX_COL_NEON_BRIGHT);

        VixRect btn_close  = {M.x + M.w - 38.0f, M.y + 12.0f, 26.0f, 26.0f};
        VixRect btn_up     = {M.x + 170.0f, M.y + 48.0f, 92.0f, 28.0f};
        VixRect btn_all    = {M.x + M.w - 280.0f, M.y + 48.0f, 110.0f, 28.0f};
        VixRect btn_sysdlg = {M.x + M.w - 162.0f, M.y + 48.0f, 146.0f, 28.0f};
        VixRect btn_open   = {M.x + M.w - 156.0f, M.y + M.h - 44.0f, 140.0f, 32.0f};
        VixRect btn_cancel = {M.x + M.w - 266.0f, M.y + M.h - 44.0f, 102.0f, 32.0f};

        draw_icon_button(r, btn_close, VIX_ICON_CLOSE, vix_rect_contains(btn_close, mx, my), false, 1.0f);
        draw_neon_button(r, btn_up, "↑ ВВЕРХ", vix_rect_contains(btn_up, mx, my), false, false, 1.0f);
        draw_neon_button(r, btn_all, ui->show_all_files ? "ВСЕ ФАЙЛЫ" : "МЕДИА",
                         vix_rect_contains(btn_all, mx, my), false, ui->show_all_files, 1.0f);
        draw_neon_button(r, btn_sysdlg, "СИСТ. ДИАЛОГ",
                         vix_rect_contains(btn_sysdlg, mx, my), false, false, 1.0f);

        float path_w = btn_all.x - (btn_up.x + btn_up.w + 16.0f);
        if (path_w > 40.0f) {
            ui_draw_text_ellipsis(r, VIX_FONT_SMALL, btn_up.x + btn_up.w + 10.0f, M.y + 55.0f,
                                  path_w, ui->browser_dir, VIX_COL_TEXT_SUB);
        }

        /* Sidebar quick folders */
        static const char *const bm_labels[5] = {
            "Домой", "Видео", "Загрузки", "Документы", "Корень /"
        };
        for (int b = 0; b < 5; b++) {
            VixRect br = {M.x + 14.0f, M.y + 84.0f + b * 36.0f, 144.0f, 30.0f};
            draw_neon_button(r, br, bm_labels[b], vix_rect_contains(br, mx, my), false, false, 1.0f);
        }

        /* Directory & File Listing */
        VixRect list_area = {M.x + 170.0f, M.y + 84.0f, M.w - 186.0f, M.h - 138.0f};
        ui_draw_box(r, list_area, vix_rgba(6, 2, 12, 0.95f), VIX_COL_BORDER_DIM, 8.0f, 1.0f, zero, 0);

        ui_push_clip(r, (VixRect){list_area.x + 2, list_area.y + 2, list_area.w - 4, list_area.h - 4});
        if (ui->dir_entry_count == 0) {
            ui_draw_text_centered(r, VIX_FONT_SMALL, list_area,
                                  "В этой папке нет подходящих медиафайлов", VIX_COL_TEXT_MUTED);
        } else {
            for (int i = 0; i < ui->dir_entry_count; i++) {
                float ry = list_area.y + i * 30.0f - ui->browser_scroll;
                if (ry + 30.0f < list_area.y || ry > list_area.y + list_area.h) continue;

                VixRect row = {list_area.x + 4.0f, ry + 2.0f, list_area.w - 8.0f, 26.0f};
                bool sel = (i == ui->selected_entry);
                bool hov = vix_rect_contains(row, mx, my);
                if (sel || hov) {
                    ui_draw_box(r, row,
                                sel ? vix_rgba(92, 28, 168, 0.88f) : vix_rgba(44, 16, 82, 0.65f),
                                sel ? VIX_COL_NEON_BRIGHT : zero, 5.0f, sel ? 1.0f : 0.0f, zero, 0);
                }

                const VixDirEntry *ent = &ui->dir_entries[i];
                ui_draw_icon(r, ent->is_dir ? VIX_ICON_FOLDER : VIX_ICON_MEDIA,
                             (VixRect){row.x + 7.0f, row.y + 5.0f, 16.0f, 16.0f},
                             ent->is_dir ? VIX_COL_NEON_PINK : VIX_COL_NEON_BRIGHT);

                ui_draw_text_ellipsis(r, VIX_FONT_SMALL, row.x + 30.0f, row.y + 6.0f,
                                      row.w - 115.0f, ent->name,
                                      ent->is_dir ? VIX_COL_TEXT_MAIN : VIX_COL_TEXT_SUB);

                if (!ent->is_dir) {
                    char sz_str[32];
                    format_filesize(ent->size_bytes, sz_str, sizeof(sz_str));
                    ui_draw_text(r, VIX_FONT_SMALL, row.x + row.w - 78.0f, row.y + 6.0f,
                                 sz_str, VIX_COL_TEXT_MUTED);
                }
            }
        }
        ui_pop_clip(r);

        ui_draw_text(r, VIX_FONT_SMALL, M.x + 18.0f, M.y + M.h - 34.0f,
                     "Двойной клик или Enter — открыть", VIX_COL_TEXT_MUTED);
        draw_neon_button(r, btn_cancel, "ОТМЕНА", vix_rect_contains(btn_cancel, mx, my), false, false, 1.0f);
        draw_neon_button(r, btn_open, "ОТКРЫТЬ", vix_rect_contains(btn_open, mx, my), true, false, 1.0f);
    }

    /* 6. Open URL / Stream Modal */
    if (ui->modal == VIX_MODAL_OPEN_URL) {
        ui_draw_box(r, (VixRect){0, 0, (float)ui->win_w, (float)ui->win_h},
                    vix_rgba(0, 0, 0, 0.68f), zero, 0, 0, zero, 0);

        VixRect M = compute_url_modal_rect(ui);
        ui_draw_box(r, M, VIX_COL_MODAL_BG, VIX_COL_NEON_LILAC,
                    14.0f, 1.5f, vix_rgba(184, 77, 255, 0.45f), 22.0f);

        ui_draw_text(r, VIX_FONT_BODY, M.x + 20.0f, M.y + 16.0f,
                     "ОТКРЫТЬ URL / ПОТОК (HTTP, HLS, YT-DLP)", VIX_COL_NEON_BRIGHT);

        VixRect btn_close = {M.x + M.w - 38.0f, M.y + 12.0f, 26.0f, 26.0f};
        draw_icon_button(r, btn_close, VIX_ICON_CLOSE, vix_rect_contains(btn_close, mx, my), false, 1.0f);

        ui_draw_text(r, VIX_FONT_SMALL, M.x + 20.0f, M.y + 54.0f,
                     "Вставьте прямую ссылку на видео или поток (Ctrl+V):", VIX_COL_TEXT_SUB);

        VixRect inp = {M.x + 20.0f, M.y + 82.0f, M.w - 40.0f, 44.0f};
        ui_draw_box(r, inp, vix_rgba(6, 2, 12, 0.98f), VIX_COL_NEON_BRIGHT,
                    8.0f, 1.4f, vix_rgba(184, 77, 255, 0.25f), 8.0f);

        if (ui->url_input[0]) {
            ui_draw_text_ellipsis(r, VIX_FONT_BODY, inp.x + 12.0f, inp.y + 13.0f,
                                  inp.w - 24.0f, ui->url_input, VIX_COL_TEXT_MAIN);
        } else {
            ui_draw_text(r, VIX_FONT_BODY, inp.x + 12.0f, inp.y + 13.0f,
                         "https://...", VIX_COL_TEXT_MUTED);
        }

        VixRect btn_paste = {M.x + 20.0f, M.y + 154.0f, 118.0f, 36.0f};
        VixRect btn_clear = {M.x + 146.0f, M.y + 154.0f, 108.0f, 36.0f};
        VixRect btn_play  = {M.x + M.w - 170.0f, M.y + 154.0f, 150.0f, 36.0f};
        draw_neon_button(r, btn_paste, "ВСТАВИТЬ", vix_rect_contains(btn_paste, mx, my), false, false, 1.0f);
        draw_neon_button(r, btn_clear, "ОЧИСТИТЬ", vix_rect_contains(btn_clear, mx, my), false, false, 1.0f);
        draw_neon_button(r, btn_play,  "ЗАПУСТИТЬ", vix_rect_contains(btn_play, mx, my), true,  false, 1.0f);
    }

    /* 7. Top-Center Neon Lilac Toast OSD */
    if (ui->anim_time < ui->toast_until && ui->toast_text[0]) {
        float rem = (float)(ui->toast_until - ui->anim_time);
        float ta = clampf(rem * 3.0f, 0.0f, 1.0f);
        float tw = fminf((float)ui->win_w - 60.0f, ui_text_width(r, VIX_FONT_BODY, ui->toast_text) + 36.0f);
        VixRect toast = {((float)ui->win_w - tw) * 0.5f, TOP_BAR_H + 16.0f, tw, 36.0f};
        ui_draw_box(r, toast, vix_alpha(vix_rgba(18, 6, 36, 0.94f), ta),
                    vix_alpha(VIX_COL_NEON_BRIGHT, ta), 18.0f, 1.3f,
                    vix_alpha(VIX_COL_GLOW_LILAC, ta * 0.65f), 14.0f);
        ui_draw_text_centered(r, VIX_FONT_BODY, toast, ui->toast_text,
                              vix_alpha(VIX_COL_TEXT_MAIN, ta));
    }
}
