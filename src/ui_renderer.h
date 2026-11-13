#ifndef VIX_UI_RENDERER_H
#define VIX_UI_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct VixColor {
    float r, g, b, a;
} VixColor;

typedef struct VixRect {
    float x, y, w, h;
} VixRect;

typedef enum VixFontSize {
    VIX_FONT_SMALL = 0, /* 13px */
    VIX_FONT_BODY  = 1, /* 15px */
    VIX_FONT_TITLE = 2, /* 24px Bold */
    VIX_FONT_COUNT = 3
} VixFontSize;

typedef enum VixIconType {
    VIX_ICON_PLAY = 0,
    VIX_ICON_PAUSE,
    VIX_ICON_STOP,
    VIX_ICON_PREV,
    VIX_ICON_NEXT,
    VIX_ICON_REWIND,
    VIX_ICON_FORWARD,
    VIX_ICON_VOLUME_HIGH,
    VIX_ICON_VOLUME_LOW,
    VIX_ICON_VOLUME_MUTE,
    VIX_ICON_FULLSCREEN,
    VIX_ICON_EXIT_FULLSCREEN,
    VIX_ICON_FOLDER,
    VIX_ICON_MEDIA,
    VIX_ICON_PLUS,
    VIX_ICON_CLOSE,
    VIX_ICON_GLITCH,
    VIX_ICON_LIST
} VixIconType;

typedef enum VixRenderBackend {
    VIX_BACKEND_OPENGL = 0,
    VIX_BACKEND_SOFTWARE = 1
} VixRenderBackend;

static inline VixColor vix_rgba(uint8_t r, uint8_t g, uint8_t b, float a) {
    VixColor c = { r / 255.0f, g / 255.0f, b / 255.0f, a };
    return c;
}

static inline VixColor vix_alpha(VixColor c, float mul) {
    c.a *= mul;
    return c;
}

static inline bool vix_rect_contains(VixRect r, float px, float py) {
    return px >= r.x && px <= (r.x + r.w) && py >= r.y && py <= (r.y + r.h);
}

/* Dark Neon Lilac Signature Palette on Pitch-Black #000000 */
#define VIX_COL_BLACK          vix_rgba(0,   0,   0,   1.00f)
#define VIX_COL_BAR_GLASS      vix_rgba(8,   3,   16,  0.90f)
#define VIX_COL_MODAL_BG       vix_rgba(11,  5,   22,  0.96f)
#define VIX_COL_CARD_BG        vix_rgba(16,  7,   31,  0.88f)
#define VIX_COL_BTN_BG         vix_rgba(24,  10,  46,  0.85f)
#define VIX_COL_BTN_HOVER      vix_rgba(52,  20,  96,  0.95f)
#define VIX_COL_BTN_ACTIVE     vix_rgba(88,  28,  155, 1.00f)
#define VIX_COL_NEON_LILAC     vix_rgba(184, 77,  255, 1.00f)
#define VIX_COL_NEON_BRIGHT    vix_rgba(220, 160, 255, 1.00f)
#define VIX_COL_NEON_PINK      vix_rgba(232, 121, 249, 1.00f)
#define VIX_COL_NEON_DEEP      vix_rgba(124, 58,  237, 1.00f)
#define VIX_COL_BORDER_DIM     vix_rgba(147, 51,  234, 0.42f)
#define VIX_COL_GLOW_LILAC     vix_rgba(176, 66,  255, 0.55f)
#define VIX_COL_TEXT_MAIN      vix_rgba(246, 238, 255, 1.00f)
#define VIX_COL_TEXT_SUB       vix_rgba(186, 158, 224, 1.00f)
#define VIX_COL_TEXT_MUTED     vix_rgba(128, 98,  170, 1.00f)
#define VIX_COL_TRACK_BG       vix_rgba(32,  14,  58,  0.92f)
#define VIX_COL_TRACK_CACHE    vix_rgba(147, 51,  234, 0.38f)

typedef struct VixUiRenderer VixUiRenderer;

VixUiRenderer *ui_renderer_create(VixRenderBackend backend, const char *app_base_dir);
void ui_renderer_destroy(VixUiRenderer *r);

void ui_begin_frame(VixUiRenderer *r, int width, int height);
void ui_end_frame(VixUiRenderer *r);

/* Clear full viewport to solid color (e.g. #000000) */
void ui_clear(VixUiRenderer *r, VixColor color);

/* Clipping scissor rect (top-left origin) */
void ui_push_clip(VixUiRenderer *r, VixRect clip);
void ui_pop_clip(VixUiRenderer *r);

/* Rounded SDF box with optional border and outer neon glow */
void ui_draw_box(VixUiRenderer *r, VixRect rect, VixColor fill,
                 VixColor border, float radius, float border_w,
                 VixColor glow, float glow_radius);

/* Rounded app emblem/icon image */
bool ui_has_logo(const VixUiRenderer *r);
void ui_draw_logo(VixUiRenderer *r, VixRect rect, float radius, float alpha);

/* Crisp vector icon inside rect */
void ui_draw_icon(VixUiRenderer *r, VixIconType icon, VixRect rect, VixColor color);

/* UTF-8 text (Latin + Cyrillic) measurement & rendering */
float ui_text_width(const VixUiRenderer *r, VixFontSize size, const char *utf8);
float ui_font_height(const VixUiRenderer *r, VixFontSize size);
void ui_draw_text(VixUiRenderer *r, VixFontSize size, float x, float y,
                  const char *utf8, VixColor color);
void ui_draw_text_ellipsis(VixUiRenderer *r, VixFontSize size, float x, float y,
                           float max_width, const char *utf8, VixColor color);
void ui_draw_text_centered(VixUiRenderer *r, VixFontSize size, VixRect box,
                           const char *utf8, VixColor color);

/* Access software RGBA pixel buffer (for fallback SDL_Surface blit or PNG preview) */
const uint8_t *ui_software_pixels(const VixUiRenderer *r, int *out_w, int *out_h);
bool ui_save_png(const VixUiRenderer *r, const char *png_path);

/* Load raw RGBA icon pixels from disk for SDL_SetWindowIcon */
uint8_t *ui_load_rgba_file(const char *path, int *out_w, int *out_h);
void ui_free_rgba_file(uint8_t *pixels);

#endif /* VIX_UI_RENDERER_H */
