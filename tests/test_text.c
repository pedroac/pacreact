/* Choosing fonts and splitting text into runs. text.c is included so the statics are reachable.
 * The checks that need real font files only run where the fonts exist. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/text.c"

static int runs_of(const char *s, Run *runs) {
    int n = split_runs(s, (int)strlen(s), runs, MAX_RUNS);
    int covered = 0;
    for (int i = 0; i < n; i++) {                 /* runs tile the string exactly */
        assert(runs[i].start == covered && runs[i].len > 0);
        covered += runs[i].len;
    }
    assert(covered == (int)strlen(s));
    return n;
}

#define THAI (SLOT_SCRIPT + 9)
#define DEVANAGARI (SLOT_SCRIPT + 0)
#define TAMIL (SLOT_SCRIPT + 4)

static bool has_font(int slot) { return get_face(slot, false)->face != NULL; }

static void test_classification(void) {
    Pick p;
    p = pick("wlan0 — online", 16);
    assert(p.slot == SLOT_LATIN && !p.shape && !p.runs);
    p = pick("Привет, мир", (int)strlen("Привет, мир"));          /* Cyrillic is in the Latin font */
    assert(p.slot == SLOT_LATIN && !p.runs);
    p = pick("مرحبا", (int)strlen("مرحبا"));
    assert(p.slot == SLOT_RTL && p.shape && p.rtl && !p.runs);
    p = pick("שלום", (int)strlen("שלום"));
    assert(p.slot == SLOT_RTL && p.shape && p.rtl);
    p = pick("日本語", (int)strlen("日本語"));
    assert(p.slot >= SLOT_CJK && p.slot < SLOT_SCRIPT && !p.shape);
    p = pick("สวัสดี", (int)strlen("สวัสดี"));                     /* Thai: per-run fonts */
    assert(p.runs && !p.shape);
    p = pick("नमस्ते", (int)strlen("नमस्ते"));
    assert(p.runs);
}

static void test_split(void) {
    Run runs[MAX_RUNS];

    assert(runs_of("wlan0", runs) == 1 && runs[0].pick.slot == SLOT_LATIN);
    assert(runs_of("สวัสดี", runs) == 1 && runs[0].pick.slot == THAI && runs[0].pick.shape);

    /* The Latin part keeps its own font: a script font often has no Latin letters */
    int n = runs_of("wlan0 — สวัสดี", runs);
    assert(n == 2 && runs[0].pick.slot == SLOT_LATIN && runs[1].pick.slot == THAI);
    assert(!strncmp("wlan0 — ", "wlan0 — ", runs[0].len) && runs[0].len == (int)strlen("wlan0 — "));

    n = runs_of("สวัสดี नमस्ते hello", runs);
    assert(n == 4);
    assert(runs[0].pick.slot == THAI && runs[1].pick.slot == SLOT_LATIN &&      /* the space */
           runs[2].pick.slot == DEVANAGARI && runs[3].pick.slot == SLOT_LATIN);

    n = runs_of("தமிழ்", runs);
    assert(n == 1 && runs[0].pick.slot == TAMIL);

    /* Many switches: capped, but still covering the whole string */
    char many[512] = "";
    for (int i = 0; i < 40; i++) strcat(many, "a" "ก");
    n = runs_of(many, runs);
    assert(n == MAX_RUNS);

    /* Every script in the table is reachable through its first code point */
    for (int i = 0; i < N_LTR_SCRIPTS; i++) {
        assert(ltr_script_of(ltr_scripts[i].lo) == i && ltr_script_of(ltr_scripts[i].hi) == i);
        assert(ltr_script_of(ltr_scripts[i].lo - 1) != i && ltr_script_of(ltr_scripts[i].hi + 1) != i);
    }
    assert(ltr_script_of('a') == -1 && ltr_script_of(0x0627) == -1);   /* Latin, Arabic */
}

static void test_with_fonts(void) {
    if (!has_font(THAI) || !has_font(SLOT_LATIN)) {
        puts("text: (font checks skipped: Noto Sans Thai or a Latin font is not installed)");
        return;
    }
    double thai = text_width("สวัสดี", (int)strlen("สวัสดี"), 16, false);
    double latin = text_width("wlan0 — ", (int)strlen("wlan0 — "), 16, false);
    const char *mixed = "wlan0 — สวัสดี";
    assert(thai > 20 && latin > 20);
    /* runs add up: the mixed string is exactly the two parts side by side */
    double both = text_width(mixed, (int)strlen(mixed), 16, false);
    assert(both > thai + latin - 0.01 && both < thai + latin + 0.01);
    /* a prefix ending between runs measures like the run */
    assert(text_width(mixed, (int)strlen("wlan0 — "), 16, false) == latin);

    /* Height covers every font in the string */
    double asc_l, h_l, asc_t, h_t, asc_m, h_m;
    text_extents("wlan0", 16, false, &asc_l, &h_l);
    text_extents("สวัสดี", 16, false, &asc_t, &h_t);
    text_extents(mixed, 16, false, &asc_m, &h_m);
    assert(asc_m >= asc_l && asc_m >= asc_t && h_m >= h_l && h_m >= h_t);

    /* And it really draws: Thai glyphs leave pixels on the surface */
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 200, 40);
    cairo_t *cr = cairo_create(img);
    cairo_set_source_rgb(cr, 1, 1, 1);
    text_draw(cr, "สวัสดี", 4, 24, 16, false);
    cairo_surface_flush(img);
    const unsigned char *px = cairo_image_surface_get_data(img);
    int painted = 0;
    for (int i = 0; i < 200 * 40 * 4; i += 4) painted += px[i + 3] != 0;
    assert(painted > 100);
    cairo_destroy(cr);
    cairo_surface_destroy(img);
}

/* PACHUD_FONT and PACHUD_FONT_BOLD: a font of the user's for Latin text. Needs two different font
 * files from the default list to tell them apart, so it only runs where there are. */
static void test_user_font(void) {
    const char *a = NULL, *b = NULL;
    for (const char *const *c = latin_fonts; *c; c++) {
        if (access(*c, R_OK) != 0) continue;
        if (!a) a = *c;
        else if (!b) b = *c;
    }
    if (!b) { puts("text: PACHUD_FONT not tested (fewer than two Latin fonts installed)"); return; }

    unsetenv("PACHUD_FONT");
    unsetenv("PACHUD_FONT_BOLD");
    memset(faces, 0, sizeof faces);
    Face *f = get_face(SLOT_LATIN, false);
    assert(f->face && !strcmp(f->path, a));                      /* the default, to compare with */

    /* One font given: it is used for regular text, and for bold the same file with bold made up */
    setenv("PACHUD_FONT", b, 1);
    memset(faces, 0, sizeof faces);
    f = get_face(SLOT_LATIN, false);
    assert(f->face && !strcmp(f->path, b));
    f = get_face(SLOT_LATIN, true);
    assert(f->face && !strcmp(f->path, b) && f != get_face(SLOT_LATIN, false));

    /* ...and it is the same size, only heavier: a face loaded with the wrong flags once came out at
     * several times the size (font units, not pixels) */
    double asc_r, h_r, asc_b, h_b;
    text_extents("A notification", 15, false, &asc_r, &h_r);
    text_extents("A notification", 15, true, &asc_b, &h_b);
    double w_r = text_width("A notification", 14, 15, false), w_b = text_width("A notification", 14, 15, true);
    assert(h_r > 5 && h_r < 30 && h_b > 5 && h_b < 30);
    assert(w_r > 60 && w_r < 200 && w_b >= w_r * 0.95 && w_b < w_r * 1.3);
    assert(asc_b < 30 && asc_r < 30);

    /* A bold file of its own is used for bold */
    setenv("PACHUD_FONT_BOLD", a, 1);
    memset(faces, 0, sizeof faces);
    assert(!strcmp(get_face(SLOT_LATIN, true)->path, a) && !strcmp(get_face(SLOT_LATIN, false)->path, b));

    /* Something that is not a font file, or not a file at all: the default, with a word about it,
     * and nothing that could block (a pipe is never opened) */
    char fifo[64];
    snprintf(fifo, sizeof fifo, "/tmp/pachud-font-%d", (int)getpid());
    assert(mkfifo(fifo, 0600) == 0);
    const char *bad[] = { "/nonexistent/font.ttf", "/tmp", fifo, "/etc/hostname", "/dev/null" };
    unsetenv("PACHUD_FONT_BOLD");
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        setenv("PACHUD_FONT", bad[i], 1);
        memset(faces, 0, sizeof faces);
        f = get_face(SLOT_LATIN, false);
        assert(f->face && !strcmp(f->path, a));
    }
    unlink(fifo);

    /* An empty value is the same as none */
    setenv("PACHUD_FONT", "", 1);
    memset(faces, 0, sizeof faces);
    assert(!strcmp(get_face(SLOT_LATIN, false)->path, a));
    unsetenv("PACHUD_FONT");
    memset(faces, 0, sizeof faces);
}

int main(void) {
    test_classification();
    test_split();
    test_with_fonts();
    test_user_font();
    puts("text: ok");
    return 0;
}
