#ifndef VIX_PLAYER_UI_H
#define VIX_PLAYER_UI_H

#include <SDL.h>
#include <stdbool.h>
#include "mpv_engine.h"
#include "ui_renderer.h"

#define VIX_MAX_DIR_ENTRIES 1024

typedef enum VixModalMode {
    VIX_MODAL_NONE = 0,
    VIX_MODAL_FILE_BROWSER,
    VIX_MODAL_OPEN_URL
} VixModalMode;

typedef struct VixDirEntry {
    char name[256];
    bool is_dir;
    bool is_subtitle;
    uint64_t size_bytes;
} VixDirEntry;

typedef struct VixPlayerUi {
    /* Window & cursor state */
    int win_w, win_h;
    float mouse_x, mouse_y;
    bool mouse_down;
    bool is_fullscreen;
    bool request_quit;
    bool request_toggle_fullscreen;

    /* Animations & auto-hide HUD */
    double anim_time;
    double last_activity_time;
    float hud_alpha;
    bool dragging_timeline;
    bool dragging_volume;

    /* Panels & overlays */
    bool show_playlist;
    bool show_info_hud;
    VixModalMode modal;

    /* Toast OSD notification */
    char toast_text[256];
    double toast_until;

    /*Playlist scroll */
    float playlist_scroll;

    /* Built-in File Browser state */
    char browser_dir[VIX_PATH_MAX];
    VixDirEntry dir_entries[VIX_MAX_DIR_ENTRIES];
    int dir_entry_count;
    int selected_entry;
    float browser_scroll;
    bool show_all_files;
    bool subtitle_pick_mode;
    uint32_t last_click_ticks;
    int last_clicked_entry;

    /* URL modal state */
    char url_input[VIX_PATH_MAX];
} VixPlayerUi;

void player_ui_init(VixPlayerUi *ui, int win_w, int win_h);
void player_ui_show_toast(VixPlayerUi *ui, const char *fmt, ...);
void player_ui_open_file_modal(VixPlayerUi *ui, bool subtitle_only);
void player_ui_open_url_modal(VixPlayerUi *ui);

void player_ui_handle_event(VixPlayerUi *ui, const SDL_Event *ev, VixMpvEngine *eng);
bool player_ui_update(VixPlayerUi *ui, double dt, const VixMpvEngine *eng);
void player_ui_render(VixPlayerUi *ui, VixUiRenderer *r, VixMpvEngine *eng);

#endif /* VIX_PLAYER_UI_H */
