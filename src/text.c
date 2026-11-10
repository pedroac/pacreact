#include "text.h"
#include "config.h"
#include "i18n.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NO_I18N
#include "shape.h"
#include "utf8.h"
#include <cairo/cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <unistd.h>
#endif

#define MAX_TEXT 1024

static cairo_t *scratch;  /* 1x1 context for measuring, created on first use */
static bool ui_rtl;

static cairo_t *scratch_cr(void) {
    if (!scratch) {
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
        scratch = cairo_create(s);
        cairo_surface_destroy(s);
    }
    return scratch;
}

bool text_rtl(void) { return ui_rtl; }

#ifdef NO_I18N

typedef struct { bool shape, rtl; } Pick;

static Pick pick(const char *utf8, int len) { (void)utf8; (void)len; return (Pick){0}; }

static void apply_face(cairo_t *cr, const Pick *p, double size, bool bold) {
    (void)p;
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

static cairo_glyph_t *shaped(const Pick *p, const char *utf8, double size, bool bold, int *n,
                             double *width) {
    (void)p; (void)utf8; (void)size; (void)bold; (void)n; (void)width;
    return NULL;
}

void text_init(void) {}
void text_finish(void) {}

#else

/* Font files are looked up directly, without fontconfig. Noto first, then DejaVu/Liberation. */
static const char *const latin_fonts[] = {
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    NULL,
};
static const char *const latin_bold_fonts[] = {
    "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    NULL,
};
/* DejaVu Sans covers Arabic and Hebrew together with Latin, so a Wi-Fi name in Latin letters
 * still renders inside right-to-left text (Noto's Arabic fonts lack Latin). */
static const char *const rtl_fonts[] = {
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    NULL,
};
static const char *const rtl_bold_fonts[] = {
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    NULL,
};
static const char *const cjk_fonts[] = {
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc",
    NULL,
};
static const char *const cjk_bold_fonts[] = {
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Bold.ttc",
    "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Bold.ttc",
    NULL,
};

typedef struct {
    const char *prefix;  /* start of the locale name, longest match first */
    bool cjk;
    int face_index;      /* face inside a .ttc: Noto CJK is JP, KR, SC, TC, HK */
    bool rtl;
} Script;

static const Script scripts[] = {
    { "ja", true, 0, false },
    { "ko", true, 1, false },
    { "zh_TW", true, 3, false },
    { "zh_HK", true, 4, false },
    { "zh", true, 2, false },
    { "ar", false, 0, true },
    { "fa", false, 0, true },
    { "he", false, 0, true },
    { "ur", false, 0, true },
};

enum { SLOT_LATIN, SLOT_RTL, SLOT_CJK, SLOT_COUNT = SLOT_CJK + 5 };  /* one CJK slot per .ttc face */

typedef struct {
    cairo_font_face_t *face;
    bool tried;
    char path[256];
    int index;
} Face;

static Face faces[SLOT_COUNT][2];  /* [slot][bold] */
static FT_Library ft;
static bool ft_ready;
static int ui_cjk_slot = -1;       /* the UI language is CJK: its Han variant, for all CJK text */

static const cairo_user_data_key_t ft_face_key;

static void done_face(void *face) { FT_Done_Face(face); }

static void load_face(Face *f, const char *const *candidates, int index) {
    f->tried = true;
    if (!ft_ready) {
        if (FT_Init_FreeType(&ft) != 0) return;
        ft_ready = true;
    }
    for (const char *const *c = candidates; *c; c++) {
        FT_Face face;
        if (access(*c, R_OK) != 0 || FT_New_Face(ft, *c, index, &face) != 0) continue;
        snprintf(f->path, sizeof f->path, "%s", *c);
        f->index = index;
        f->face = cairo_ft_font_face_create_for_ft_face(face, 0);
        cairo_font_face_set_user_data(f->face, &ft_face_key, face, done_face);
        return;
    }
}

static Face *get_face(int slot, bool bold) {
    Face *f = &faces[slot][bold];
    if (!f->tried) {
        const char *const *list = slot == SLOT_LATIN ? (bold ? latin_bold_fonts : latin_fonts)
                                : slot == SLOT_RTL   ? (bold ? rtl_bold_fonts : rtl_fonts)
                                                     : (bold ? cjk_bold_fonts : cjk_fonts);
        load_face(f, list, slot >= SLOT_CJK ? slot - SLOT_CJK : 0);
    }
    if (!f->face && bold) return get_face(slot, false);  /* no bold file: regular will do */
    return f;
}

typedef struct { int slot; bool shape, rtl; } Pick;

static Pick pick(const char *utf8, int len) {
    Pick p = { .slot = SLOT_LATIN, .rtl = ui_rtl };
    bool complex = false, cjk = false, hangul = false, kana = false, strong_seen = false;

    for (int i = 0; i < len;) {
        uint32_t cp;
        int n = utf8_decode(utf8 + i, &cp);
        if (!n) { i++; continue; }
        i += n;

        bool arabic_hebrew = (cp >= 0x0590 && cp < 0x0900) || (cp >= 0xFB1D && cp <= 0xFDFF) ||
                             (cp >= 0xFE70 && cp <= 0xFEFF);
        if (arabic_hebrew || (cp >= 0x0900 && cp < 0x1100)) {  /* RTL scripts, Indic, Thai, ... */
            complex = true;
            if (!strong_seen) { strong_seen = true; p.rtl = arabic_hebrew; }
        } else if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF)) {
            hangul = true;
        } else if (cp >= 0x3040 && cp <= 0x30FF) {
            kana = true;
        } else if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                   (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FFFF)) {
            cjk = true;
        } else if (!strong_seen && ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z')) {
            strong_seen = true;  /* text that starts in Latin letters is left to right */
            p.rtl = false;
        }
    }

    if (complex) {
        p.slot = SLOT_RTL;
        p.shape = true;
    } else if (cjk || hangul || kana) {
        int variant = ui_cjk_slot >= 0 ? ui_cjk_slot - SLOT_CJK : hangul ? 1 : kana ? 0 : 2;
        p.slot = SLOT_CJK + variant;
    }
    return p;
}

static void apply_face(cairo_t *cr, const Pick *p, double size, bool bold) {
    Face *f = get_face(p->slot, bold);
    if (f->face) cairo_set_font_face(cr, f->face);
    else cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                                bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

/* Glyphs positioned by HarfBuzz/FriBidi for text that needs joining or reordering, or NULL
 * if the text does not, or no font file is available. The caller frees them. */
static cairo_glyph_t *shaped(const Pick *p, const char *utf8, double size, bool bold, int *n,
                             double *width) {
    if (!p->shape) return NULL;
    Face *f = get_face(p->slot, bold);
    if (!f->face) return NULL;
    return shape_text(f->path, f->index, size, p->rtl, utf8, n, width);
}

static const Script *script_for_locale(void) {
    const char *locale = setlocale(LC_MESSAGES, NULL);
    bool messages_c = !locale || !strcmp(locale, "C") || !strcmp(locale, "POSIX");

    /* gettext prefers the first LANGUAGE entry unless the locale is plain "C". */
    const char *language = getenv("LANGUAGE");
    if (!messages_c && language && *language && *language != ':') locale = language;
    if (!locale) return NULL;

    for (size_t i = 0; i < sizeof scripts / sizeof *scripts; i++)
        if (!strncmp(locale, scripts[i].prefix, strlen(scripts[i].prefix))) return &scripts[i];
    return NULL;
}

void text_init(void) {
    /* Only what is displayed is localized; other categories would just cost memory. */
    setlocale(LC_MESSAGES, "");
    bindtextdomain("pachud", LOCALEDIR);
    bind_textdomain_codeset("pachud", "UTF-8");
    textdomain("pachud");

    const Script *s = script_for_locale();
    ui_rtl = s && s->rtl;
    if (s && s->cjk) ui_cjk_slot = SLOT_CJK + s->face_index;
}

void text_finish(void) {
    shape_finish();
    if (scratch) cairo_destroy(scratch);
    scratch = NULL;
}

#endif /* NO_I18N */

/* Copies up to MAX_TEXT-1 bytes without cutting a UTF-8 sequence in half. */
static int copy_text(char *out, const char *utf8, int len) {
    if (len > MAX_TEXT - 1) len = MAX_TEXT - 1;
    while (len > 0 && ((unsigned char)utf8[len] & 0xC0) == 0x80) len--;
    memcpy(out, utf8, len);
    out[len] = '\0';
    return len;
}

double text_width(const char *utf8, int len, double size, bool bold) {
    char tmp[MAX_TEXT];
    len = copy_text(tmp, utf8, len);
    Pick p = pick(tmp, len);
    cairo_t *cr = scratch_cr();
    apply_face(cr, &p, size, bold);

    int n;
    double width;
    cairo_glyph_t *g = shaped(&p, tmp, size, bold, &n, &width);
    if (g) {
        free(g);
        return width;
    }
    cairo_text_extents_t te;
    cairo_text_extents(cr, tmp, &te);
    return te.x_advance;
}

void text_extents(const char *utf8, double size, bool bold, double *ascent, double *height) {
    char tmp[MAX_TEXT];
    int len = copy_text(tmp, utf8, (int)strlen(utf8));
    Pick p = pick(tmp, len);
    cairo_t *cr = scratch_cr();
    apply_face(cr, &p, size, bold);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    *ascent = fe.ascent;
    *height = fe.height;
}

void text_draw(cairo_t *cr, const char *utf8, double x, double baseline, double size, bool bold) {
    char tmp[MAX_TEXT];
    int len = copy_text(tmp, utf8, (int)strlen(utf8));
    Pick p = pick(tmp, len);
    apply_face(cr, &p, size, bold);

    int n;
    double width;
    cairo_glyph_t *g = shaped(&p, tmp, size, bold, &n, &width);
    if (g) {
        for (int i = 0; i < n; i++) {
            g[i].x += x;
            g[i].y += baseline;
        }
        cairo_show_glyphs(cr, g, n);
        free(g);
        return;
    }
    cairo_move_to(cr, x, baseline);
    cairo_show_text(cr, tmp);
}
