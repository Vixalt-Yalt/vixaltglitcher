#define _GNU_SOURCE
#include "ui_renderer.h"
#include "gl_loader.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ATLAS_W 1024
#define ATLAS_H 1024
#define MAX_BATCH_VERTS 16384

typedef struct GlyphEntry {
    uint32_t codepoint;
    int x0, y0, x1, y1;
    float xoff, yoff, xadvance;
    bool valid;
} GlyphEntry;

#define GLYPH_CAP 320

typedef struct BakedFont {
    float pixel_height;
    float ascent;
    float descent;
    float line_height;
    GlyphEntry glyphs[GLYPH_CAP];
    int glyph_count;
} BakedFont;

typedef struct UiVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
} UiVertex;

struct VixUiRenderer {
    VixRenderBackend backend;
    int fb_w, fb_h;
    bool clip_active;
    VixRect clip_rect;

    /* Font atlas (RGBA 1024x1024) */
    uint8_t *atlas_rgba;
    BakedFont fonts[VIX_FONT_COUNT];
    bool fonts_ready;

    /* Logo RGBA */
    uint8_t *logo_rgba;
    int logo_w, logo_h;

    /* OpenGL objects */
    GLuint prog;
    GLuint vao;
    GLuint vbo;
    GLuint tex_atlas;
    GLuint tex_logo;
    GLuint tex_white;

    GLint u_res;
    GLint u_mode;
    GLint u_tex;
    GLint u_box_rect;
    GLint u_box_params; /* x=radius, y=border_w, z=glow_radius, w=unused */
    GLint u_fill_col;
    GLint u_border_col;
    GLint u_glow_col;

    UiVertex verts[MAX_BATCH_VERTS];
    int vert_count;

    /* Software backend RGBA buffer */
    uint8_t *sw_rgba;
    int sw_cap_w, sw_cap_h;
};

static uint32_t next_utf8_cp(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    if (!*s) return 0;
    uint32_t cp = 0;
    if (s[0] < 0x80) {
        cp = s[0];
        *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) |
             ((uint32_t)(s[1] & 0x3F) << 6) |
             (uint32_t)(s[2] & 0x3F);
        *p += 3;
    } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x07) << 18) |
             ((uint32_t)(s[1] & 0x3F) << 12) |
             ((uint32_t)(s[2] & 0x3F) << 6) |
             (uint32_t)(s[3] & 0x3F);
        *p += 4;
    } else {
        cp = '?';
        *p += 1;
    }
    return cp;
}

static uint8_t *read_entire_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

static bool find_asset_file(const char *app_base_dir, const char *rel_repo,
                            const char *rel_share, char *out, size_t out_sz) {
    const char *prefixes[4] = { app_base_dir, ".", "/usr", NULL };
    for (int i = 0; prefixes[i]; i++) {
        if (!*prefixes[i]) continue;
        snprintf(out, out_sz, "%s/%s", prefixes[i], rel_repo);
        FILE *f = fopen(out, "rb");
        if (f) { fclose(f); return true; }

        snprintf(out, out_sz, "%s/../share/vixglitcher/%s", prefixes[i], rel_share);
        f = fopen(out, "rb");
        if (f) { fclose(f); return true; }

        snprintf(out, out_sz, "%s/share/vixglitcher/%s", prefixes[i], rel_share);
        f = fopen(out, "rb");
        if (f) { fclose(f); return true; }
    }
    return false;
}

static void build_codepoint_list(uint32_t *cps, int *out_count) {
    int n = 0;
    for (uint32_t c = 0x0020; c <= 0x007E; c++) cps[n++] = c;
    for (uint32_t c = 0x00A0; c <= 0x00FF; c++) cps[n++] = c;
    for (uint32_t c = 0x0400; c <= 0x045F; c++) cps[n++] = c;
    static const uint32_t extras[] = {
        0x2013, 0x2014, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193
    };
    for (size_t i = 0; i < sizeof(extras) / sizeof(extras[0]); i++) {
        cps[n++] = extras[i];
    }
    *out_count = n;
}

static bool bake_fonts_into_atlas(VixUiRenderer *r, const char *base_dir) {
    char medium_path[2048], bold_path[2048];
    if (!find_asset_file(base_dir, "assets/fonts/JetBrainsMono-Medium.ttf",
                         "fonts/JetBrainsMono-Medium.ttf", medium_path, sizeof(medium_path))) {
        return false;
    }
    if (!find_asset_file(base_dir, "assets/fonts/JetBrainsMono-Bold.ttf",
                         "fonts/JetBrainsMono-Bold.ttf", bold_path, sizeof(bold_path))) {
        snprintf(bold_path, sizeof(bold_path), "%s", medium_path);
    }

    size_t med_len = 0, bold_len = 0;
    uint8_t *med_ttf = read_entire_file(medium_path, &med_len);
    uint8_t *bold_ttf = read_entire_file(bold_path, &bold_len);
    if (!med_ttf || !bold_ttf) {
        free(med_ttf);
        free(bold_ttf);
        return false;
    }

    stbtt_fontinfo fi_med, fi_bold;
    if (!stbtt_InitFont(&fi_med, med_ttf, 0) || !stbtt_InitFont(&fi_bold, bold_ttf, 0)) {
        free(med_ttf);
        free(bold_ttf);
        return false;
    }

    uint8_t *alpha_bitmap = (uint8_t *)calloc(ATLAS_W * ATLAS_H, 1);
    r->atlas_rgba = (uint8_t *)calloc(ATLAS_W * ATLAS_H * 4, 1);
    if (!alpha_bitmap || !r->atlas_rgba) {
        free(alpha_bitmap);
        free(med_ttf);
        free(bold_ttf);
        return false;
    }

    uint32_t cps[GLYPH_CAP];
    int cp_count = 0;
    build_codepoint_list(cps, &cp_count);

    const float sizes[VIX_FONT_COUNT] = { 13.5f, 15.5f, 24.0f };
    int pen_x = 2, pen_y = 2, row_h = 0;

    for (int f = 0; f < VIX_FONT_COUNT; f++) {
        stbtt_fontinfo *fi = (f == VIX_FONT_TITLE) ? &fi_bold : &fi_med;
        BakedFont *bf = &r->fonts[f];
        bf->pixel_height = sizes[f];
        float scale = stbtt_ScaleForPixelHeight(fi, sizes[f]);

        int asc, desc, gap;
        stbtt_GetFontVMetrics(fi, &asc, &desc, &gap);
        bf->ascent = asc * scale;
        bf->descent = desc * scale;
        bf->line_height = (asc - desc + gap) * scale;
        bf->glyph_count = cp_count;

        for (int i = 0; i < cp_count; i++) {
            uint32_t cp = cps[i];
            GlyphEntry *ge = &bf->glyphs[i];
            memset(ge, 0, sizeof(*ge));
            ge->codepoint = cp;

            int glyph_idx = stbtt_FindGlyphIndex(fi, (int)cp);
            if (glyph_idx == 0 && cp != ' ') {
                glyph_idx = stbtt_FindGlyphIndex(fi, '?');
            }

            int adv, lsb;
            stbtt_GetGlyphHMetrics(fi, glyph_idx, &adv, &lsb);
            ge->xadvance = adv * scale;

            int ix0, iy0, ix1, iy1;
            stbtt_GetGlyphBitmapBox(fi, glyph_idx, scale, scale, &ix0, &iy0, &ix1, &iy1);
            int gw = ix1 - ix0;
            int gh = iy1 - iy0;

            if (gw > 0 && gh > 0) {
                if (pen_x + gw + 2 >= ATLAS_W) {
                    pen_x = 2;
                    pen_y += row_h + 2;
                    row_h = 0;
                }
                if (pen_y + gh + 2 < ATLAS_H) {
                    stbtt_MakeGlyphBitmap(fi, alpha_bitmap + pen_y * ATLAS_W + pen_x,
                                          gw, gh, ATLAS_W, scale, scale, glyph_idx);
                    ge->x0 = pen_x;
                    ge->y0 = pen_y;
                    ge->x1 = pen_x + gw;
                    ge->y1 = pen_y + gh;
                    pen_x += gw + 2;
                    if (gh > row_h) row_h = gh;
                }
            }
            ge->xoff = (float)ix0;
            ge->yoff = (float)iy0;
            ge->valid = true;
        }
    }

    for (int i = 0; i < ATLAS_W * ATLAS_H; i++) {
        r->atlas_rgba[i * 4 + 0] = 255;
        r->atlas_rgba[i * 4 + 1] = 255;
        r->atlas_rgba[i * 4 + 2] = 255;
        r->atlas_rgba[i * 4 + 3] = alpha_bitmap[i];
    }

    free(alpha_bitmap);
    free(med_ttf);
    free(bold_ttf);
    r->fonts_ready = true;
    return true;
}

static const GlyphEntry *lookup_glyph(const BakedFont *bf, uint32_t cp) {
    for (int i = 0; i < bf->glyph_count; i++) {
        if (bf->glyphs[i].codepoint == cp) return &bf->glyphs[i];
    }
    for (int i = 0; i < bf->glyph_count; i++) {
        if (bf->glyphs[i].codepoint == '?') return &bf->glyphs[i];
    }
    return NULL;
}

/* ---------------- OpenGL Shader Setup ---------------- */

static GLuint compile_gl_shader(GLenum type, const char *src) {
    GLuint s = gl.CreateShader(type);
    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        gl.GetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "[VIXGlitcher] Shader compile error: %s\n", log);
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

static bool init_gl_pipeline(VixUiRenderer *r) {
    const char *vs_150 =
        "#version 150 core\n"
        "in vec2 a_pos;\n"
        "in vec2 a_uv;\n"
        "in vec4 a_col;\n"
        "uniform vec2 u_res;\n"
        "out vec2 v_pos;\n"
        "out vec2 v_uv;\n"
        "out vec4 v_col;\n"
        "void main() {\n"
        "    v_pos = a_pos;\n"
        "    v_uv = a_uv;\n"
        "    v_col = a_col;\n"
        "    vec2 ndc = vec2((a_pos.x / u_res.x) * 2.0 - 1.0, 1.0 - (a_pos.y / u_res.y) * 2.0);\n"
        "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
        "}\n";

    const char *fs_150 =
        "#version 150 core\n"
        "in vec2 v_pos;\n"
        "in vec2 v_uv;\n"
        "in vec4 v_col;\n"
        "uniform int u_mode;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_box_rect;\n"
        "uniform vec4 u_box_params;\n"
        "uniform vec4 u_fill_col;\n"
        "uniform vec4 u_border_col;\n"
        "uniform vec4 u_glow_col;\n"
        "out vec4 fragColor;\n"
        "float sdRoundBox(vec2 p, vec2 b, float r) {\n"
        "    vec2 q = abs(p) - b + vec2(r);\n"
        "    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"
        "}\n"
        "void main() {\n"
        "    if (u_mode == 0) {\n"
        "        vec2 half_sz = u_box_rect.zw * 0.5;\n"
        "        vec2 center = u_box_rect.xy + half_sz;\n"
        "        float rad = min(u_box_params.x, min(half_sz.x, half_sz.y));\n"
        "        float bw = u_box_params.y;\n"
        "        float gr = u_box_params.z;\n"
        "        float d = sdRoundBox(v_pos - center, half_sz, rad);\n"
        "        float fill_a = 1.0 - smoothstep(-0.6, 0.6, d);\n"
        "        vec4 col = vec4(u_fill_col.rgb, u_fill_col.a * fill_a);\n"
        "        if (bw > 0.01) {\n"
        "            float ba = (1.0 - smoothstep(-0.6, 0.6, d)) * smoothstep(-bw - 0.8, -bw + 0.4, d);\n"
        "            float sa = u_border_col.a * ba;\n"
        "            col.rgb = mix(col.rgb, u_border_col.rgb, sa / max(col.a + sa * (1.0 - col.a), 0.0001));\n"
        "            col.a = col.a + sa * (1.0 - col.a);\n"
        "        }\n"
        "        if (gr > 0.5 && d > -0.5) {\n"
        "            float gf = exp(-max(d, 0.0) / (gr * 0.42)) * (1.0 - smoothstep(0.0, gr, max(d, 0.0)));\n"
        "            float ga = u_glow_col.a * gf * (1.0 - fill_a * 0.75);\n"
        "            col.rgb = mix(col.rgb, u_glow_col.rgb, ga / max(col.a + ga * (1.0 - col.a), 0.0001));\n"
        "            col.a = col.a + ga * (1.0 - col.a);\n"
        "        }\n"
        "        fragColor = col;\n"
        "    } else if (u_mode == 1) {\n"
        "        vec4 t = texture(u_tex, v_uv);\n"
        "        fragColor = vec4(v_col.rgb, v_col.a * t.a);\n"
        "    } else if (u_mode == 2) {\n"
        "        vec2 half_sz = u_box_rect.zw * 0.5;\n"
        "        vec2 center = u_box_rect.xy + half_sz;\n"
        "        float rad = min(u_box_params.x, min(half_sz.x, half_sz.y));\n"
        "        float d = sdRoundBox(v_pos - center, half_sz, rad);\n"
        "        float mask = 1.0 - smoothstep(-0.7, 0.7, d);\n"
        "        vec4 t = texture(u_tex, v_uv);\n"
        "        fragColor = vec4(t.rgb, t.a * mask * v_col.a);\n"
        "    } else {\n"
        "        fragColor = v_col;\n"
        "    }\n"
        "}\n";

    const char *vs_120 =
        "#version 120\n"
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "attribute vec4 a_col;\n"
        "uniform vec2 u_res;\n"
        "varying vec2 v_pos;\n"
        "varying vec2 v_uv;\n"
        "varying vec4 v_col;\n"
        "void main() {\n"
        "    v_pos = a_pos;\n"
        "    v_uv = a_uv;\n"
        "    v_col = a_col;\n"
        "    vec2 ndc = vec2((a_pos.x / u_res.x) * 2.0 - 1.0, 1.0 - (a_pos.y / u_res.y) * 2.0);\n"
        "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
        "}\n";

    const char *fs_120 =
        "#version 120\n"
        "varying vec2 v_pos;\n"
        "varying vec2 v_uv;\n"
        "varying vec4 v_col;\n"
        "uniform int u_mode;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_box_rect;\n"
        "uniform vec4 u_box_params;\n"
        "uniform vec4 u_fill_col;\n"
        "uniform vec4 u_border_col;\n"
        "uniform vec4 u_glow_col;\n"
        "float sdRoundBox(vec2 p, vec2 b, float r) {\n"
        "    vec2 q = abs(p) - b + vec2(r);\n"
        "    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;\n"
        "}\n"
        "void main() {\n"
        "    if (u_mode == 0) {\n"
        "        vec2 half_sz = u_box_rect.zw * 0.5;\n"
        "        vec2 center = u_box_rect.xy + half_sz;\n"
        "        float rad = min(u_box_params.x, min(half_sz.x, half_sz.y));\n"
        "        float bw = u_box_params.y;\n"
        "        float gr = u_box_params.z;\n"
        "        float d = sdRoundBox(v_pos - center, half_sz, rad);\n"
        "        float fill_a = 1.0 - smoothstep(-0.6, 0.6, d);\n"
        "        vec4 col = vec4(u_fill_col.rgb, u_fill_col.a * fill_a);\n"
        "        if (bw > 0.01) {\n"
        "            float ba = (1.0 - smoothstep(-0.6, 0.6, d)) * smoothstep(-bw - 0.8, -bw + 0.4, d);\n"
        "            float sa = u_border_col.a * ba;\n"
        "            col.rgb = mix(col.rgb, u_border_col.rgb, sa / max(col.a + sa * (1.0 - col.a), 0.0001));\n"
        "            col.a = col.a + sa * (1.0 - col.a);\n"
        "        }\n"
        "        if (gr > 0.5 && d > -0.5) {\n"
        "            float gf = exp(-max(d, 0.0) / (gr * 0.42)) * (1.0 - smoothstep(0.0, gr, max(d, 0.0)));\n"
        "            float ga = u_glow_col.a * gf * (1.0 - fill_a * 0.75);\n"
        "            col.rgb = mix(col.rgb, u_glow_col.rgb, ga / max(col.a + ga * (1.0 - col.a), 0.0001));\n"
        "            col.a = col.a + ga * (1.0 - col.a);\n"
        "        }\n"
        "        gl_FragColor = col;\n"
        "    } else if (u_mode == 1) {\n"
        "        vec4 t = texture2D(u_tex, v_uv);\n"
        "        gl_FragColor = vec4(v_col.rgb, v_col.a * t.a);\n"
        "    } else if (u_mode == 2) {\n"
        "        vec2 half_sz = u_box_rect.zw * 0.5;\n"
        "        vec2 center = u_box_rect.xy + half_sz;\n"
        "        float rad = min(u_box_params.x, min(half_sz.x, half_sz.y));\n"
        "        float d = sdRoundBox(v_pos - center, half_sz, rad);\n"
        "        float mask = 1.0 - smoothstep(-0.7, 0.7, d);\n"
        "        vec4 t = texture2D(u_tex, v_uv);\n"
        "        gl_FragColor = vec4(t.rgb, t.a * mask * v_col.a);\n"
        "    } else {\n"
        "        gl_FragColor = v_col;\n"
        "    }\n"
        "}\n";

    GLuint vs = compile_gl_shader(GL_VERTEX_SHADER, gl.is_core_profile ? vs_150 : vs_120);
    GLuint fs = compile_gl_shader(GL_FRAGMENT_SHADER, gl.is_core_profile ? fs_150 : fs_120);
    if (!vs || !fs) {
        if (vs) gl.DeleteShader(vs);
        if (fs) gl.DeleteShader(fs);
        return false;
    }

    r->prog = gl.CreateProgram();
    gl.AttachShader(r->prog, vs);
    gl.AttachShader(r->prog, fs);
    gl.BindAttribLocation(r->prog, 0, "a_pos");
    gl.BindAttribLocation(r->prog, 1, "a_uv");
    gl.BindAttribLocation(r->prog, 2, "a_col");
    gl.LinkProgram(r->prog);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);

    GLint linked = 0;
    gl.GetProgramiv(r->prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        gl.DeleteProgram(r->prog);
        r->prog = 0;
        return false;
    }

    r->u_res        = gl.GetUniformLocation(r->prog, "u_res");
    r->u_mode       = gl.GetUniformLocation(r->prog, "u_mode");
    r->u_tex        = gl.GetUniformLocation(r->prog, "u_tex");
    r->u_box_rect   = gl.GetUniformLocation(r->prog, "u_box_rect");
    r->u_box_params = gl.GetUniformLocation(r->prog, "u_box_params");
    r->u_fill_col   = gl.GetUniformLocation(r->prog, "u_fill_col");
    r->u_border_col = gl.GetUniformLocation(r->prog, "u_border_col");
    r->u_glow_col   = gl.GetUniformLocation(r->prog, "u_glow_col");

    if (gl.GenVertexArrays) {
        gl.GenVertexArrays(1, &r->vao);
        gl.BindVertexArray(r->vao);
    }
    gl.GenBuffers(1, &r->vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl.BufferData(GL_ARRAY_BUFFER, sizeof(r->verts), NULL, GL_DYNAMIC_DRAW);

    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (r->atlas_rgba) {
        gl.GenTextures(1, &r->tex_atlas);
        gl.BindTexture(GL_TEXTURE_2D, r->tex_atlas);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_W, ATLAS_H, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, r->atlas_rgba);
    }

    if (r->logo_rgba && r->logo_w > 0 && r->logo_h > 0) {
        gl.GenTextures(1, &r->tex_logo);
        gl.BindTexture(GL_TEXTURE_2D, r->tex_logo);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, r->logo_w, r->logo_h, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, r->logo_rgba);
    }

    uint8_t white[4] = {255, 255, 255, 255};
    gl.GenTextures(1, &r->tex_white);
    gl.BindTexture(GL_TEXTURE_2D, r->tex_white);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);

    return true;
}

static void bind_gl_ui_state(VixUiRenderer *r) {
    if (gl.BindFramebuffer) gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Viewport(0, 0, r->fb_w, r->fb_h);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_CULL_FACE);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (r->clip_active) {
        gl.Enable(GL_SCISSOR_TEST);
        int sy = r->fb_h - (int)(r->clip_rect.y + r->clip_rect.h);
        gl.Scissor((int)r->clip_rect.x, sy < 0 ? 0 : sy,
                   (int)r->clip_rect.w, (int)r->clip_rect.h);
    } else {
        gl.Disable(GL_SCISSOR_TEST);
    }

    gl.UseProgram(r->prog);
    gl.Uniform2f(r->u_res, (float)r->fb_w, (float)r->fb_h);
    gl.Uniform1i(r->u_tex, 0);
    gl.ActiveTexture(GL_TEXTURE0);

    if (gl.BindVertexArray && r->vao) gl.BindVertexArray(r->vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl.EnableVertexAttribArray(0);
    gl.EnableVertexAttribArray(1);
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex), (const void *)offsetof(UiVertex, x));
    gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex), (const void *)offsetof(UiVertex, u));
    gl.VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(UiVertex), (const void *)offsetof(UiVertex, r));
}

static void gl_draw_quad_verts(VixUiRenderer *r, const UiVertex v[6]) {
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, sizeof(UiVertex) * 6, v);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
}

/* ---------------- Software Backend Helpers ---------------- */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline float smoothstepf(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline float sd_round_box_sw(float px, float py, float bx, float by, float rad) {
    float qx = fabsf(px) - bx + rad;
    float qy = fabsf(py) - by + rad;
    float mx = qx > 0.0f ? qx : 0.0f;
    float my = qy > 0.0f ? qy : 0.0f;
    float in_part = fminf(fmaxf(qx, qy), 0.0f);
    return in_part + sqrtf(mx * mx + my * my) - rad;
}

static inline void sw_blend_pixel(VixUiRenderer *r, int x, int y, float sr, float sg, float sb, float sa) {
    if (sa <= 0.001f) return;
    if (x < 0 || y < 0 || x >= r->fb_w || y >= r->fb_h) return;
    if (r->clip_active) {
        if (x < (int)r->clip_rect.x || x >= (int)(r->clip_rect.x + r->clip_rect.w) ||
            y < (int)r->clip_rect.y || y >= (int)(r->clip_rect.y + r->clip_rect.h))
            return;
    }
    sa = clampf(sa, 0.0f, 1.0f);
    uint8_t *dst = &r->sw_rgba[(y * r->fb_w + x) * 4];
    float dr = dst[0] / 255.0f;
    float dg = dst[1] / 255.0f;
    float db = dst[2] / 255.0f;
    float inv = 1.0f - sa;
    dst[0] = (uint8_t)(clampf(sr * sa + dr * inv, 0.0f, 1.0f) * 255.0f + 0.5f);
    dst[1] = (uint8_t)(clampf(sg * sa + dg * inv, 0.0f, 1.0f) * 255.0f + 0.5f);
    dst[2] = (uint8_t)(clampf(sb * sa + db * inv, 0.0f, 1.0f) * 255.0f + 0.5f);
    dst[3] = 255;
}

static void sw_draw_triangle(VixUiRenderer *r, float x0, float y0, float x1, float y1,
                             float x2, float y2, VixColor col) {
    int min_x = (int)floorf(fminf(x0, fminf(x1, x2))) - 1;
    int max_x = (int)ceilf(fmaxf(x0, fmaxf(x1, x2))) + 1;
    int min_y = (int)floorf(fminf(y0, fminf(y1, y2))) - 1;
    int max_y = (int)ceilf(fmaxf(y0, fmaxf(y1, y2))) + 1;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= r->fb_w) max_x = r->fb_w - 1;
    if (max_y >= r->fb_h) max_y = r->fb_h - 1;

    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (fabsf(area) < 0.001f) return;

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            /* 2x2 MSAA supersampling for razor-sharp software icons */
            int hits = 0;
            static const float offs[4][2] = {{0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.75f}};
            for (int s = 0; s < 4; s++) {
                float px = x + offs[s][0];
                float py = y + offs[s][1];
                float w0 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
                float w1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
                float w2 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);
                if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                    (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                    hits++;
                }
            }
            if (hits > 0) {
                sw_blend_pixel(r, x, y, col.r, col.g, col.b, col.a * (hits * 0.25f));
            }
        }
    }
}

/* ---------------- Public Lifecycle & Drawing API ---------------- */

VixUiRenderer *ui_renderer_create(VixRenderBackend backend, const char *app_base_dir) {
    VixUiRenderer *r = (VixUiRenderer *)calloc(1, sizeof(VixUiRenderer));
    if (!r) return NULL;
    r->backend = backend;

    bake_fonts_into_atlas(r, app_base_dir ? app_base_dir : ".");

    char icon_path[2048];
    if (find_asset_file(app_base_dir ? app_base_dir : ".",
                        "assets/icons/vixglitcher-256.png",
                        "icons/vixglitcher-256.png", icon_path, sizeof(icon_path))) {
        int ch = 0;
        r->logo_rgba = stbi_load(icon_path, &r->logo_w, &r->logo_h, &ch, 4);
    }

    if (backend == VIX_BACKEND_OPENGL) {
        if (!init_gl_pipeline(r)) {
            r->backend = VIX_BACKEND_SOFTWARE;
        }
    }
    return r;
}

void ui_renderer_destroy(VixUiRenderer *r) {
    if (!r) return;
    if (r->backend == VIX_BACKEND_OPENGL && gl.loaded) {
        if (r->tex_atlas) gl.DeleteTextures(1, &r->tex_atlas);
        if (r->tex_logo) gl.DeleteTextures(1, &r->tex_logo);
        if (r->tex_white) gl.DeleteTextures(1, &r->tex_white);
        if (r->vbo) gl.DeleteBuffers(1, &r->vbo);
        if (r->vao && gl.DeleteVertexArrays) gl.DeleteVertexArrays(1, &r->vao);
        if (r->prog) gl.DeleteProgram(r->prog);
    }
    free(r->atlas_rgba);
    if (r->logo_rgba) stbi_image_free(r->logo_rgba);
    free(r->sw_rgba);
    free(r);
}

void ui_begin_frame(VixUiRenderer *r, int width, int height) {
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    r->fb_w = width;
    r->fb_h = height;
    r->clip_active = false;

    if (r->backend == VIX_BACKEND_SOFTWARE) {
        if (width != r->sw_cap_w || height != r->sw_cap_h || !r->sw_rgba) {
            free(r->sw_rgba);
            r->sw_cap_w = width;
            r->sw_cap_h = height;
            r->sw_rgba = (uint8_t *)calloc((size_t)width * (size_t)height * 4, 1);
        }
    } else {
        bind_gl_ui_state(r);
    }
}

void ui_end_frame(VixUiRenderer *r) {
    if (r->backend == VIX_BACKEND_OPENGL) {
        gl.Disable(GL_SCISSOR_TEST);
    }
}

void ui_clear(VixUiRenderer *r, VixColor color) {
    if (r->backend == VIX_BACKEND_OPENGL) {
        if (gl.BindFramebuffer) gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.Viewport(0, 0, r->fb_w, r->fb_h);
        gl.Disable(GL_SCISSOR_TEST);
        gl.ClearColor(color.r, color.g, color.b, color.a);
        gl.Clear(GL_COLOR_BUFFER_BIT);
        bind_gl_ui_state(r);
    } else if (r->sw_rgba) {
        uint8_t cr = (uint8_t)(clampf(color.r, 0.0f, 1.0f) * 255.0f);
        uint8_t cg = (uint8_t)(clampf(color.g, 0.0f, 1.0f) * 255.0f);
        uint8_t cb = (uint8_t)(clampf(color.b, 0.0f, 1.0f) * 255.0f);
        int total = r->fb_w * r->fb_h;
        for (int i = 0; i < total; i++) {
            r->sw_rgba[i * 4 + 0] = cr;
            r->sw_rgba[i * 4 + 1] = cg;
            r->sw_rgba[i * 4 + 2] = cb;
            r->sw_rgba[i * 4 + 3] = 255;
        }
    }
}

void ui_push_clip(VixUiRenderer *r, VixRect clip) {
    r->clip_active = true;
    r->clip_rect = clip;
    if (r->backend == VIX_BACKEND_OPENGL) {
        gl.Enable(GL_SCISSOR_TEST);
        int sy = r->fb_h - (int)(clip.y + clip.h);
        gl.Scissor((int)clip.x, sy < 0 ? 0 : sy,
                   (int)(clip.w < 0 ? 0 : clip.w), (int)(clip.h < 0 ? 0 : clip.h));
    }
}

void ui_pop_clip(VixUiRenderer *r) {
    r->clip_active = false;
    if (r->backend == VIX_BACKEND_OPENGL) {
        gl.Disable(GL_SCISSOR_TEST);
    }
}

void ui_draw_box(VixUiRenderer *r, VixRect rect, VixColor fill,
                 VixColor border, float radius, float border_w,
                 VixColor glow, float glow_radius) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) return;
    float pad = (glow_radius > 0.5f && glow.a > 0.005f) ? glow_radius : 1.5f;
    float x0 = rect.x - pad;
    float y0 = rect.y - pad;
    float x1 = rect.x + rect.w + pad;
    float y1 = rect.y + rect.h + pad;

    if (r->backend == VIX_BACKEND_OPENGL) {
        gl.Uniform1i(r->u_mode, 0);
        gl.BindTexture(GL_TEXTURE_2D, r->tex_white);
        gl.Uniform4f(r->u_box_rect, rect.x, rect.y, rect.w, rect.h);
        gl.Uniform4f(r->u_box_params, radius, border_w, glow.a > 0.005f ? glow_radius : 0.0f, 0.0f);
        gl.Uniform4f(r->u_fill_col, fill.r, fill.g, fill.b, fill.a);
        gl.Uniform4f(r->u_border_col, border.r, border.g, border.b, border.a);
        gl.Uniform4f(r->u_glow_col, glow.r, glow.g, glow.b, glow.a);

        UiVertex q[6] = {
            {x0, y0, 0, 0, 1, 1, 1, 1}, {x1, y0, 1, 0, 1, 1, 1, 1}, {x1, y1, 1, 1, 1, 1, 1, 1},
            {x0, y0, 0, 0, 1, 1, 1, 1}, {x1, y1, 1, 1, 1, 1, 1, 1}, {x0, y1, 0, 1, 1, 1, 1, 1},
        };
        gl_draw_quad_verts(r, q);
    } else if (r->sw_rgba) {
        int ix0 = (int)floorf(x0); if (ix0 < 0) ix0 = 0;
        int iy0 = (int)floorf(y0); if (iy0 < 0) iy0 = 0;
        int ix1 = (int)ceilf(x1);  if (ix1 >= r->fb_w) ix1 = r->fb_w - 1;
        int iy1 = (int)ceilf(y1);  if (iy1 >= r->fb_h) iy1 = r->fb_h - 1;

        float hx = rect.w * 0.5f, hy = rect.h * 0.5f;
        float cx = rect.x + hx, cy = rect.y + hy;
        float rad = fminf(radius, fminf(hx, hy));
        float gr = (glow.a > 0.005f) ? glow_radius : 0.0f;

        for (int py = iy0; py <= iy1; py++) {
            for (int px = ix0; px <= ix1; px++) {
                float d = sd_round_box_sw((px + 0.5f) - cx, (py + 0.5f) - cy, hx, hy, rad);
                float fill_a = 1.0f - smoothstepf(-0.6f, 0.6f, d);
                float cr = fill.r, cg = fill.g, cb = fill.b, ca = fill.a * fill_a;

                if (border_w > 0.01f && border.a > 0.002f) {
                    float ba = (1.0f - smoothstepf(-0.6f, 0.6f, d)) *
                               smoothstepf(-border_w - 0.8f, -border_w + 0.4f, d);
                    float sa = border.a * ba;
                    float out_a = ca + sa * (1.0f - ca);
                    if (out_a > 0.0001f) {
                        float t = sa / out_a;
                        cr = cr + (border.r - cr) * t;
                        cg = cg + (border.g - cg) * t;
                        cb = cb + (border.b - cb) * t;
                    }
                    ca = out_a;
                }
                if (gr > 0.5f && d > -0.5f) {
                    float dp = d > 0.0f ? d : 0.0f;
                    float gf = expf(-dp / (gr * 0.42f)) * (1.0f - smoothstepf(0.0f, gr, dp));
                    float ga = glow.a * gf * (1.0f - fill_a * 0.75f);
                    float out_a = ca + ga * (1.0f - ca);
                    if (out_a > 0.0001f) {
                        float t = ga / out_a;
                        cr = cr + (glow.r - cr) * t;
                        cg = cg + (glow.g - cg) * t;
                        cb = cb + (glow.b - cb) * t;
                    }
                    ca = out_a;
                }
                sw_blend_pixel(r, px, py, cr, cg, cb, ca);
            }
        }
    }
}

bool ui_has_logo(const VixUiRenderer *r) {
    return r && r->logo_rgba != NULL && r->logo_w > 0 && r->logo_h > 0;
}

void ui_draw_logo(VixUiRenderer *r, VixRect rect, float radius, float alpha) {
    if (!ui_has_logo(r) || alpha <= 0.002f) return;

    if (r->backend == VIX_BACKEND_OPENGL && r->tex_logo) {
        gl.Uniform1i(r->u_mode, 2);
        gl.BindTexture(GL_TEXTURE_2D, r->tex_logo);
        gl.Uniform4f(r->u_box_rect, rect.x, rect.y, rect.w, rect.h);
        gl.Uniform4f(r->u_box_params, radius, 0.0f, 0.0f, 0.0f);

        float x0 = rect.x, y0 = rect.y, x1 = rect.x + rect.w, y1 = rect.y + rect.h;
        UiVertex q[6] = {
            {x0, y0, 0, 0, 1, 1, 1, alpha}, {x1, y0, 1, 0, 1, 1, 1, alpha}, {x1, y1, 1, 1, 1, 1, 1, alpha},
            {x0, y0, 0, 0, 1, 1, 1, alpha}, {x1, y1, 1, 1, 1, 1, 1, alpha}, {x0, y1, 0, 1, 1, 1, 1, alpha},
        };
        gl_draw_quad_verts(r, q);
    } else if (r->sw_rgba) {
        int ix0 = (int)floorf(rect.x); if (ix0 < 0) ix0 = 0;
        int iy0 = (int)floorf(rect.y); if (iy0 < 0) iy0 = 0;
        int ix1 = (int)ceilf(rect.x + rect.w); if (ix1 >= r->fb_w) ix1 = r->fb_w - 1;
        int iy1 = (int)ceilf(rect.y + rect.h); if (iy1 >= r->fb_h) iy1 = r->fb_h - 1;

        float hx = rect.w * 0.5f, hy = rect.h * 0.5f;
        float cx = rect.x + hx, cy = rect.y + hy;
        float rad = fminf(radius, fminf(hx, hy));

        for (int py = iy0; py <= iy1; py++) {
            for (int px = ix0; px <= ix1; px++) {
                float d = sd_round_box_sw((px + 0.5f) - cx, (py + 0.5f) - cy, hx, hy, rad);
                float mask = 1.0f - smoothstepf(-0.7f, 0.7f, d);
                if (mask <= 0.002f) continue;

                float u0 = clampf((px - rect.x) / rect.w, 0.0f, 0.9999f) * r->logo_w;
                float v0 = clampf((py - rect.y) / rect.h, 0.0f, 0.9999f) * r->logo_h;
                float u1 = clampf((px + 1.0f - rect.x) / rect.w, 0.0f, 0.9999f) * r->logo_w;
                float v1 = clampf((py + 1.0f - rect.y) / rect.h, 0.0f, 0.9999f) * r->logo_h;
                int sx0 = (int)u0, sy0 = (int)v0;
                int sx1 = (int)u1, sy1 = (int)v1;
                if (sx1 < sx0) sx1 = sx0;
                if (sy1 < sy0) sy1 = sy0;
                float ar = 0, ag = 0, ab = 0, aa = 0;
                int cnt = 0;
                for (int sy = sy0; sy <= sy1 && sy < r->logo_h; sy++) {
                    for (int sx = sx0; sx <= sx1 && sx < r->logo_w; sx++) {
                        const uint8_t *sp = &r->logo_rgba[(sy * r->logo_w + sx) * 4];
                        ar += sp[0]; ag += sp[1]; ab += sp[2]; aa += sp[3];
                        cnt++;
                    }
                }
                if (cnt > 0) {
                    float inv = 1.0f / (255.0f * cnt);
                    sw_blend_pixel(r, px, py, ar * inv, ag * inv, ab * inv,
                                   (aa * inv) * mask * alpha);
                }
            }
        }
    }
}

static void emit_solid_tri(VixUiRenderer *r, float x0, float y0, float x1, float y1,
                           float x2, float y2, VixColor c) {
    if (r->backend == VIX_BACKEND_OPENGL) {
        gl.Uniform1i(r->u_mode, 3);
        gl.BindTexture(GL_TEXTURE_2D, r->tex_white);
        UiVertex t[3] = {
            {x0, y0, 0, 0, c.r, c.g, c.b, c.a},
            {x1, y1, 0, 0, c.r, c.g, c.b, c.a},
            {x2, y2, 0, 0, c.r, c.g, c.b, c.a},
        };
        gl.BufferSubData(GL_ARRAY_BUFFER, 0, sizeof(t), t);
        gl.DrawArrays(GL_TRIANGLES, 0, 3);
    } else {
        sw_draw_triangle(r, x0, y0, x1, y1, x2, y2, c);
    }
}

static void emit_line_seg(VixUiRenderer *r, float x0, float y0, float x1, float y1,
                          float thickness, VixColor c) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = (-dy / len) * (thickness * 0.5f);
    float ny = (dx / len) * (thickness * 0.5f);
    emit_solid_tri(r, x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    emit_solid_tri(r, x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

void ui_draw_icon(VixUiRenderer *r, VixIconType icon, VixRect box, VixColor c) {
    VixColor zero = {0, 0, 0, 0};
    float cx = box.x + box.w * 0.5f;
    float cy = box.y + box.h * 0.5f;
    float s  = fminf(box.w, box.h) * 0.5f;

    switch (icon) {
        case VIX_ICON_PLAY:
            emit_solid_tri(r, cx - s * 0.45f, cy - s * 0.65f,
                              cx + s * 0.68f, cy,
                              cx - s * 0.45f, cy + s * 0.65f, c);
            break;

        case VIX_ICON_PAUSE: {
            float bw = fmaxf(2.5f, s * 0.34f);
            float bh = s * 1.26f;
            VixRect b1 = { cx - s * 0.52f, cy - bh * 0.5f, bw, bh };
            VixRect b2 = { cx + s * 0.52f - bw, cy - bh * 0.5f, bw, bh };
            ui_draw_box(r, b1, c, zero, 1.5f, 0, zero, 0);
            ui_draw_box(r, b2, c, zero, 1.5f, 0, zero, 0);
            break;
        }

        case VIX_ICON_STOP: {
            float sz = s * 1.12f;
            VixRect b = { cx - sz * 0.5f, cy - sz * 0.5f, sz, sz };
            ui_draw_box(r, b, c, zero, 2.0f, 0, zero, 0);
            break;
        }

        case VIX_ICON_PREV: {
            VixRect bar = { cx - s * 0.65f, cy - s * 0.60f, fmaxf(2.0f, s * 0.22f), s * 1.20f };
            ui_draw_box(r, bar, c, zero, 1.0f, 0, zero, 0);
            emit_solid_tri(r, cx + s * 0.62f, cy - s * 0.60f,
                              cx - s * 0.35f, cy,
                              cx + s * 0.62f, cy + s * 0.60f, c);
            break;
        }

        case VIX_ICON_NEXT: {
            VixRect bar = { cx + s * 0.43f, cy - s * 0.60f, fmaxf(2.0f, s * 0.22f), s * 1.20f };
            ui_draw_box(r, bar, c, zero, 1.0f, 0, zero, 0);
            emit_solid_tri(r, cx - s * 0.62f, cy - s * 0.60f,
                              cx + s * 0.35f, cy,
                              cx - s * 0.62f, cy + s * 0.60f, c);
            break;
        }

        case VIX_ICON_REWIND:
            emit_solid_tri(r, cx, cy - s * 0.56f, cx - s * 0.72f, cy, cx, cy + s * 0.56f, c);
            emit_solid_tri(r, cx + s * 0.68f, cy - s * 0.56f, cx - s * 0.04f, cy, cx + s * 0.68f, cy + s * 0.56f, c);
            break;

        case VIX_ICON_FORWARD:
            emit_solid_tri(r, cx - s * 0.68f, cy - s * 0.56f, cx + s * 0.04f, cy, cx - s * 0.68f, cy + s * 0.56f, c);
            emit_solid_tri(r, cx, cy - s * 0.56f, cx + s * 0.72f, cy, cx, cy + s * 0.56f, c);
            break;

        case VIX_ICON_VOLUME_HIGH:
        case VIX_ICON_VOLUME_LOW:
        case VIX_ICON_VOLUME_MUTE: {
            VixRect base = { cx - s * 0.72f, cy - s * 0.28f, s * 0.38f, s * 0.56f };
            ui_draw_box(r, base, c, zero, 1.0f, 0, zero, 0);
            emit_solid_tri(r, cx - s * 0.42f, cy - s * 0.28f,
                              cx + s * 0.05f, cy - s * 0.68f,
                              cx + s * 0.05f, cy + s * 0.68f, c);
            emit_solid_tri(r, cx - s * 0.42f, cy - s * 0.28f,
                              cx + s * 0.05f, cy + s * 0.68f,
                              cx - s * 0.42f, cy + s * 0.28f, c);
            if (icon == VIX_ICON_VOLUME_MUTE) {
                emit_line_seg(r, cx + s * 0.22f, cy - s * 0.38f, cx + s * 0.74f, cy + s * 0.38f, 2.0f, c);
                emit_line_seg(r, cx + s * 0.74f, cy - s * 0.38f, cx + s * 0.22f, cy + s * 0.38f, 2.0f, c);
            } else {
                emit_line_seg(r, cx + s * 0.26f, cy - s * 0.30f, cx + s * 0.36f, cy, 1.8f, c);
                emit_line_seg(r, cx + s * 0.36f, cy, cx + s * 0.26f, cy + s * 0.30f, 1.8f, c);
                if (icon == VIX_ICON_VOLUME_HIGH) {
                    emit_line_seg(r, cx + s * 0.52f, cy - s * 0.52f, cx + s * 0.68f, cy, 1.8f, c);
                    emit_line_seg(r, cx + s * 0.68f, cy, cx + s * 0.52f, cy + s * 0.52f, 1.8f, c);
                }
            }
            break;
        }

        case VIX_ICON_FULLSCREEN:
        case VIX_ICON_EXIT_FULLSCREEN: {
            float o = s * 0.65f, l = s * 0.38f, t = 2.0f;
            emit_line_seg(r, cx - o, cy - o, cx - o + l, cy - o, t, c);
            emit_line_seg(r, cx - o, cy - o, cx - o, cy - o + l, t, c);
            emit_line_seg(r, cx + o, cy - o, cx + o - l, cy - o, t, c);
            emit_line_seg(r, cx + o, cy - o, cx + o, cy - o + l, t, c);
            emit_line_seg(r, cx - o, cy + o, cx - o + l, cy + o, t, c);
            emit_line_seg(r, cx - o, cy + o, cx - o, cy + o - l, t, c);
            emit_line_seg(r, cx + o, cy + o, cx + o - l, cy + o, t, c);
            emit_line_seg(r, cx + o, cy + o, cx + o, cy + o - l, t, c);
            break;
        }

        case VIX_ICON_FOLDER: {
            VixRect tab = { cx - s * 0.72f, cy - s * 0.58f, s * 0.62f, s * 0.32f };
            VixRect body = { cx - s * 0.72f, cy - s * 0.38f, s * 1.44f, s * 1.00f };
            ui_draw_box(r, tab, c, zero, 2.0f, 0, zero, 0);
            ui_draw_box(r, body, c, zero, 2.5f, 0, zero, 0);
            break;
        }

        case VIX_ICON_MEDIA: {
            VixRect frame = { cx - s * 0.68f, cy - s * 0.54f, s * 1.36f, s * 1.08f };
            ui_draw_box(r, zero.a ? frame : frame, zero, c, 2.5f, 1.6f, zero, 0);
            emit_solid_tri(r, cx - s * 0.20f, cy - s * 0.28f,
                              cx + s * 0.32f, cy,
                              cx - s * 0.20f, cy + s * 0.28f, c);
            break;
        }

        case VIX_ICON_PLUS:
            emit_line_seg(r, cx - s * 0.55f, cy, cx + s * 0.55f, cy, 2.0f, c);
            emit_line_seg(r, cx, cy - s * 0.55f, cx, cy + s * 0.55f, 2.0f, c);
            break;

        case VIX_ICON_CLOSE:
            emit_line_seg(r, cx - s * 0.50f, cy - s * 0.50f, cx + s * 0.50f, cy + s * 0.50f, 2.0f, c);
            emit_line_seg(r, cx + s * 0.50f, cy - s * 0.50f, cx - s * 0.50f, cy + s * 0.50f, 2.0f, c);
            break;

        case VIX_ICON_GLITCH:
            emit_line_seg(r, cx + s * 0.12f, cy - s * 0.70f, cx - s * 0.35f, cy + s * 0.05f, 2.0f, c);
            emit_line_seg(r, cx - s * 0.35f, cy + s * 0.05f, cx + s * 0.30f, cy - s * 0.05f, 2.0f, c);
            emit_line_seg(r, cx + s * 0.30f, cy - s * 0.05f, cx - s * 0.12f, cy + s * 0.70f, 2.0f, c);
            break;

        case VIX_ICON_LIST:
            for (int i = -1; i <= 1; i++) {
                float ry = cy + i * s * 0.45f;
                VixRect ln = { cx - s * 0.65f, ry - 1.0f, s * 1.30f, 2.0f };
                ui_draw_box(r, ln, c, zero, 1.0f, 0, zero, 0);
            }
            break;
    }
}

float ui_font_height(const VixUiRenderer *r, VixFontSize size) {
    if (!r || !r->fonts_ready) return 15.0f;
    return r->fonts[size].pixel_height;
}

float ui_text_width(const VixUiRenderer *r, VixFontSize size, const char *utf8) {
    if (!r || !r->fonts_ready || !utf8) return 0.0f;
    const BakedFont *bf = &r->fonts[size];
    float w = 0.0f;
    const char *p = utf8;
    uint32_t cp;
    while ((cp = next_utf8_cp(&p)) != 0) {
        const GlyphEntry *ge = lookup_glyph(bf, cp);
        if (ge) w += ge->xadvance;
    }
    return w;
}

void ui_draw_text(VixUiRenderer *r, VixFontSize size, float x, float y,
                  const char *utf8, VixColor color) {
    if (!r || !r->fonts_ready || !utf8 || !*utf8 || color.a <= 0.002f) return;
    const BakedFont *bf = &r->fonts[size];
    float pen_x = floorf(x + 0.5f);
    float baseline_y = floorf(y + bf->ascent + 0.5f);

    if (r->backend == VIX_BACKEND_OPENGL) {
        gl.Uniform1i(r->u_mode, 1);
        gl.BindTexture(GL_TEXTURE_2D, r->tex_atlas);
        r->vert_count = 0;

        const char *p = utf8;
        uint32_t cp;
        while ((cp = next_utf8_cp(&p)) != 0) {
            const GlyphEntry *ge = lookup_glyph(bf, cp);
            if (!ge) continue;
            int gw = ge->x1 - ge->x0;
            int gh = ge->y1 - ge->y0;
            if (gw > 0 && gh > 0 && r->vert_count + 6 <= MAX_BATCH_VERTS) {
                float x0 = pen_x + ge->xoff;
                float y0 = baseline_y + ge->yoff;
                float x1 = x0 + gw;
                float y1 = y0 + gh;
                float u0 = (float)ge->x0 / ATLAS_W;
                float v0 = (float)ge->y0 / ATLAS_H;
                float u1 = (float)ge->x1 / ATLAS_W;
                float v1 = (float)ge->y1 / ATLAS_H;

                UiVertex *v = &r->verts[r->vert_count];
                v[0] = (UiVertex){x0, y0, u0, v0, color.r, color.g, color.b, color.a};
                v[1] = (UiVertex){x1, y0, u1, v0, color.r, color.g, color.b, color.a};
                v[2] = (UiVertex){x1, y1, u1, v1, color.r, color.g, color.b, color.a};
                v[3] = (UiVertex){x0, y0, u0, v0, color.r, color.g, color.b, color.a};
                v[4] = (UiVertex){x1, y1, u1, v1, color.r, color.g, color.b, color.a};
                v[5] = (UiVertex){x0, y1, u0, v1, color.r, color.g, color.b, color.a};
                r->vert_count += 6;
            }
            pen_x += ge->xadvance;
        }
        if (r->vert_count > 0) {
            gl.BufferSubData(GL_ARRAY_BUFFER, 0, sizeof(UiVertex) * r->vert_count, r->verts);
            gl.DrawArrays(GL_TRIANGLES, 0, r->vert_count);
        }
    } else if (r->sw_rgba && r->atlas_rgba) {
        const char *p = utf8;
        uint32_t cp;
        while ((cp = next_utf8_cp(&p)) != 0) {
            const GlyphEntry *ge = lookup_glyph(bf, cp);
            if (!ge) continue;
            int gw = ge->x1 - ge->x0;
            int gh = ge->y1 - ge->y0;
            int dx0 = (int)floorf(pen_x + ge->xoff);
            int dy0 = (int)floorf(baseline_y + ge->yoff);
            for (int gy = 0; gy < gh; gy++) {
                for (int gx = 0; gx < gw; gx++) {
                    uint8_t cov = r->atlas_rgba[((ge->y0 + gy) * ATLAS_W + (ge->x0 + gx)) * 4 + 3];
                    if (cov > 0) {
                        sw_blend_pixel(r, dx0 + gx, dy0 + gy, color.r, color.g, color.b,
                                       color.a * (cov / 255.0f));
                    }
                }
            }
            pen_x += ge->xadvance;
        }
    }
}

void ui_draw_text_ellipsis(VixUiRenderer *r, VixFontSize size, float x, float y,
                           float max_width, const char *utf8, VixColor color) {
    if (!utf8 || max_width <= 8.0f) return;
    if (ui_text_width(r, size, utf8) <= max_width) {
        ui_draw_text(r, size, x, y, utf8, color);
        return;
    }

    char buf[1024];
    float dots_w = ui_text_width(r, size, "...");
    float budget = max_width - dots_w;
    if (budget <= 0.0f) return;

    const BakedFont *bf = &r->fonts[size];
    const char *p = utf8;
    const char *last_good = utf8;
    float acc = 0.0f;
    while (*p) {
        const char *prev = p;
        uint32_t cp = next_utf8_cp(&p);
        if (!cp) break;
        const GlyphEntry *ge = lookup_glyph(bf, cp);
        float adv = ge ? ge->xadvance : 8.0f;
        if (acc + adv > budget) break;
        acc += adv;
        last_good = p;
        (void)prev;
    }
    size_t n = (size_t)(last_good - utf8);
    if (n > sizeof(buf) - 5) n = sizeof(buf) - 5;
    memcpy(buf, utf8, n);
    memcpy(buf + n, "...", 4);
    ui_draw_text(r, size, x, y, buf, color);
}

void ui_draw_text_centered(VixUiRenderer *r, VixFontSize size, VixRect box,
                           const char *utf8, VixColor color) {
    float tw = ui_text_width(r, size, utf8);
    float th = ui_font_height(r, size);
    float tx = box.x + (box.w - tw) * 0.5f;
    float ty = box.y + (box.h - th) * 0.5f - 1.0f;
    ui_draw_text(r, size, tx, ty, utf8, color);
}

const uint8_t *ui_software_pixels(const VixUiRenderer *r, int *out_w, int *out_h) {
    if (!r || !r->sw_rgba) return NULL;
    if (out_w) *out_w = r->fb_w;
    if (out_h) *out_h = r->fb_h;
    return r->sw_rgba;
}

bool ui_save_png(const VixUiRenderer *r, const char *png_path) {
    if (!r || !r->sw_rgba || !png_path) return false;
    return stbi_write_png(png_path, r->fb_w, r->fb_h, 4, r->sw_rgba, r->fb_w * 4) != 0;
}

uint8_t *ui_load_rgba_file(const char *path, int *out_w, int *out_h) {
    int ch = 0;
    return stbi_load(path, out_w, out_h, &ch, 4);
}

void ui_free_rgba_file(uint8_t *pixels) {
    if (pixels) stbi_image_free(pixels);
}
