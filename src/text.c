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
#include <sys/stat.h>
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

typedef struct { bool shape, rtl, runs; } Pick;

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

/* Scripts written left to right that need shaping and have a Noto font of their own. Each is a
 * separate font slot; text that mixes them with Latin is split into runs (see split_runs). */
typedef struct { uint32_t lo, hi; const char *font; } LtrScript;
static const LtrScript ltr_scripts[] = {
    { 0x0900, 0x097F, "NotoSansDevanagari" }, { 0x0980, 0x09FF, "NotoSansBengali" },
    { 0x0A00, 0x0A7F, "NotoSansGurmukhi" },   { 0x0A80, 0x0AFF, "NotoSansGujarati" },
    { 0x0B80, 0x0BFF, "NotoSansTamil" },      { 0x0C00, 0x0C7F, "NotoSansTelugu" },
    { 0x0C80, 0x0CFF, "NotoSansKannada" },    { 0x0D00, 0x0D7F, "NotoSansMalayalam" },
    { 0x0D80, 0x0DFF, "NotoSansSinhala" },    { 0x0E00, 0x0E7F, "NotoSansThai" },
};
#define N_LTR_SCRIPTS ((int)(sizeof ltr_scripts / sizeof *ltr_scripts))

static int ltr_script_of(uint32_t cp) {
    for (int i = 0; i < N_LTR_SCRIPTS; i++)
        if (cp >= ltr_scripts[i].lo && cp <= ltr_scripts[i].hi) return i;
    return -1;
}

enum { SLOT_LATIN, SLOT_RTL, SLOT_CJK, SLOT_SCRIPT = SLOT_CJK + 5,   /* one CJK slot per .ttc face */
       SLOT_COUNT = SLOT_SCRIPT + 10 };

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

static void load_face(Face *f, const char *const *candidates, int index, int synthesize) {
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
        f->face = cairo_ft_font_face_create_for_ft_face(face, 0);   /* (0: FreeType's load flags) */
        if (synthesize) cairo_ft_font_face_set_synthesize(f->face, synthesize);
        cairo_font_face_set_user_data(f->face, &ft_face_key, face, done_face);
        return;
    }
}

/* PACHUD_FONT: a font file for Latin text instead of the default (the other scripts keep theirs).
 * PACHUD_FONT_BOLD is its bold; without one, bold text is the same file with the bold made by
 * the renderer. Anything that is not a readable regular file is said so once and ignored. */
static void load_user_face(Face *f, bool bold) {
    const char *path = getenv(bold ? "PACHUD_FONT_BOLD" : "PACHUD_FONT");
    int flags = 0;
    if (bold && !(path && *path)) {
        path = getenv("PACHUD_FONT");
        flags = CAIRO_FT_SYNTHESIZE_BOLD;
    }
    if (!path || !*path) return;
    struct stat st;
    const char *list[] = { path, NULL };
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) load_face(f, list, 0, flags);
    static bool warned[2];
    if (!f->face && !warned[bold]) {
        warned[bold] = true;
        fprintf(stderr, "pachud: cannot use the font \"%s\" (not a font file that can be read); using the default\n", path);
    }
}

static Face *get_face(int slot, bool bold) {
    Face *f = &faces[slot][bold];
    if (!f->tried && slot >= SLOT_SCRIPT) {
        char a[256], b[256];
        const char *name = ltr_scripts[slot - SLOT_SCRIPT].font, *weight = bold ? "Bold" : "Regular";
        snprintf(a, sizeof a, "/usr/share/fonts/noto/%s-%s.ttf", name, weight);
        snprintf(b, sizeof b, "/usr/share/fonts/truetype/noto/%s-%s.ttf", name, weight);
        const char *list[] = { a, b, NULL };
        load_face(f, list, 0, 0);
    } else if (!f->tried) {
        const char *const *list = slot == SLOT_LATIN ? (bold ? latin_bold_fonts : latin_fonts)
                                : slot == SLOT_RTL   ? (bold ? rtl_bold_fonts : rtl_fonts)
                                                     : (bold ? cjk_bold_fonts : cjk_fonts);
        if (slot == SLOT_LATIN) load_user_face(f, bold);   /* PACHUD_FONT, if there is one and it works */
        if (!f->face) load_face(f, list, slot >= SLOT_CJK ? slot - SLOT_CJK : 0, 0);
    }
    if (!f->face && bold) return get_face(slot, false);  /* no bold file: regular will do */
    return f;
}

typedef struct { int slot; bool shape, rtl, runs; } Pick;

static Pick pick(const char *utf8, int len) {
    Pick p = { .slot = SLOT_LATIN, .rtl = ui_rtl };
    bool complex = false, ltr_script = false, cjk = false, hangul = false, kana = false, strong_seen = false;

    for (int i = 0; i < len;) {
        uint32_t cp;
        int n = utf8_decode(utf8 + i, &cp);
        if (!n) { i++; continue; }
        i += n;

        bool arabic_hebrew = (cp >= 0x0590 && cp < 0x0900) || (cp >= 0xFB1D && cp <= 0xFDFF) ||
                             (cp >= 0xFE70 && cp <= 0xFEFF);
        if (!arabic_hebrew && ltr_script_of(cp) >= 0) {
            ltr_script = true;     /* Indic and Thai: their own fonts, run by run */
        } else if (arabic_hebrew || (cp >= 0x0900 && cp < 0x1100)) {  /* RTL scripts, other complex ones */
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
    } else if (ltr_script) {
        p.runs = true;
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

typedef struct { int start, len; Pick pick; } Run;
#define MAX_RUNS 24

/* Cuts text into runs that each use one font: a run of one Indic or Thai script, or a run of
 * everything else (Latin, digits, punctuation) in the ordinary font. A script's own font often
 * lacks Latin letters, so "wlan0" inside Thai text must not be drawn with it. Anything beyond
 * MAX_RUNS stays in the last run. */
static int split_runs(const char *s, int len, Run *runs, int max) {
    int n = 0, cls = -2, start = 0;  /* cls: -1 ordinary, otherwise the script index */
    for (int i = 0; i < len;) {
        uint32_t cp;
        int step = utf8_decode(s + i, &cp);
        if (!step) { step = 1; cp = 0; }
        int c = ltr_script_of(cp);
        if (cls != -2 && c != cls && n < max - 1) {
            runs[n++] = (Run){ start, i - start, {0} };
            runs[n - 1].pick = cls < 0 ? (Pick){ .slot = SLOT_LATIN }
                                       : (Pick){ .slot = SLOT_SCRIPT + cls, .shape = true };
            start = i;
        }
        cls = c;
        i += step;
    }
    runs[n++] = (Run){ start, len - start, {0} };
    runs[n - 1].pick = cls < 0 ? (Pick){ .slot = SLOT_LATIN }
                               : (Pick){ .slot = SLOT_SCRIPT + cls, .shape = true };
    return n;
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

#ifdef NO_I18N
#define MAX_RUNS 1
typedef struct { int start, len; Pick pick; } Run;
static int split_runs(const char *s, int len, Run *runs, int max) {
    (void)s; (void)max;
    runs[0] = (Run){ 0, len, {0} };
    return 1;
}
#endif

/* Copies up to MAX_TEXT-1 bytes without cutting a UTF-8 sequence in half. */
static int copy_text(char *out, const char *utf8, int len) {
    if (len > MAX_TEXT - 1) len = MAX_TEXT - 1;
    while (len > 0 && ((unsigned char)utf8[len] & 0xC0) == 0x80) len--;
    memcpy(out, utf8, len);
    out[len] = '\0';
    return len;
}

/* The runs of a string: normally one, several for text that mixes Indic or Thai scripts. */
static int runs_for(const char *tmp, int len, Run *runs) {
    Pick p = pick(tmp, len);
    if (!p.runs) {
        runs[0] = (Run){ 0, len, p };
        return 1;
    }
    return split_runs(tmp, len, runs, MAX_RUNS);
}

static int run_text(char *out, const char *tmp, const Run *r) {
    memcpy(out, tmp + r->start, r->len);
    out[r->len] = '\0';
    return r->len;
}

static double run_width(cairo_t *cr, const char *text, const Pick *p, double size, bool bold) {
    apply_face(cr, p, size, bold);
    int n;
    double width;
    cairo_glyph_t *g = shaped(p, text, size, bold, &n, &width);
    if (g) {
        free(g);
        return width;
    }
    cairo_text_extents_t te;
    cairo_text_extents(cr, text, &te);
    return te.x_advance;
}

double text_width(const char *utf8, int len, double size, bool bold) {
    char tmp[MAX_TEXT], part[MAX_TEXT];
    len = copy_text(tmp, utf8, len);
    Run runs[MAX_RUNS];
    int n = runs_for(tmp, len, runs);
    cairo_t *cr = scratch_cr();
    double total = 0;
    for (int i = 0; i < n; i++) {
        run_text(part, tmp, &runs[i]);
        total += run_width(cr, part, &runs[i].pick, size, bold);
    }
    return total;
}

void text_extents(const char *utf8, double size, bool bold, double *ascent, double *height) {
    char tmp[MAX_TEXT];
    int len = copy_text(tmp, utf8, (int)strlen(utf8));
    Run runs[MAX_RUNS];
    int n = runs_for(tmp, len, runs);
    cairo_t *cr = scratch_cr();
    *ascent = *height = 0;
    for (int i = 0; i < n; i++) {  /* tall enough for every font used */
        apply_face(cr, &runs[i].pick, size, bold);
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        if (fe.ascent > *ascent) *ascent = fe.ascent;
        if (fe.height > *height) *height = fe.height;
    }
}

void text_draw(cairo_t *cr, const char *utf8, double x, double baseline, double size, bool bold) {
    char tmp[MAX_TEXT], part[MAX_TEXT];
    int len = copy_text(tmp, utf8, (int)strlen(utf8));
    Run runs[MAX_RUNS];
    int n = runs_for(tmp, len, runs);

    for (int i = 0; i < n; i++) {
        run_text(part, tmp, &runs[i]);
        const Pick *p = &runs[i].pick;
        apply_face(cr, p, size, bold);

        int count;
        double width;
        cairo_glyph_t *g = shaped(p, part, size, bold, &count, &width);
        if (g) {
            for (int k = 0; k < count; k++) {
                g[k].x += x;
                g[k].y += baseline;
            }
            cairo_show_glyphs(cr, g, count);
            free(g);
        } else {
            cairo_text_extents_t te;
            cairo_text_extents(cr, part, &te);
            width = te.x_advance;
            cairo_move_to(cr, x, baseline);
            cairo_show_text(cr, part);
        }
        x += width;
    }
}
