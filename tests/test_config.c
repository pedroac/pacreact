/* The settings file: keys become settings, a later file overrides an earlier one, bad lines are skipped. */
#include "../src/settings_file.h"
#include <assert.h>
#include <stdbool.h>
#include "setting.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0);
    fclose(f);
}

/* Every key the file accepts must appear in the example, commented out or not. */
static bool conf_has_key(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '#') p++;
        size_t n = strcspn(p, " \t=\n");
        if (n == strlen(key) && strncmp(p, key, n) == 0) found = true;
    }
    fclose(f);
    return found;
}

int main(void) {
    char path[] = "/tmp/pacreact-conf-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    write_file(path,
        "# a comment\n"
        "\n"
        "font_size = 22   # trailing comment\n"
        "Peek-Idle=7\n"
        "font = /usr/share/fonts/My Font.ttf\n"
        "width = 400\n"
        "no equals sign\n"
        "bogus = 1\n"
        "sound =\n"
        "PATH = /nowhere\n");

    assert(settings_load_file(path) == 3);              /* no '=', bogus, PATH */
    assert(strcmp(pac_setting("font_size"), "22") == 0);
    assert(strcmp(pac_setting("peek_idle"), "7") == 0);          /* case and '-' forgiven */
    assert(strcmp(pac_setting("font"), "/usr/share/fonts/My Font.ttf") == 0);
    assert(strcmp(pac_setting("width"), "400") == 0);
    assert(strcmp(pac_setting("sound"), "") == 0);                /* an empty value is a value */
    assert(!pac_setting("bogus"));
    assert(!pac_setting("path"));                       /* only pacreact's own settings */
    assert(!getenv("PACREACT_FONT_SIZE"));              /* and never the environment */

    /* a file read later overrides the earlier */
    write_file(path, "width = 300\n");
    assert(settings_load_file(path) == 0);
    assert(strcmp(pac_setting("width"), "300") == 0);
    assert(strcmp(pac_setting("font_size"), "22") == 0);          /* what it does not say stays */

    unlink(path);
    assert(settings_load_file(path) == -1);             /* no file: nothing happens */

    /* The example in the project folder is commented out at its defaults (principle 23), so
     * reading it changes nothing. */
    assert(settings_load_file("pacreact.conf") == 0);
    assert(strcmp(pac_setting("width"), "300") == 0);
    assert(strcmp(pac_setting("font_size"), "22") == 0);

    /* A number outside its range is refused where it is read, with the file and line. */
    write_file(path, "margin = 500\nscale = 9\nfont_size = 99\n"
                     "anchor = middle\nicons = gtk\nprint = maybe\nprobe_host = not-an-ip\n"
                     "usb = camera,bogus\n");
    assert(settings_load_file(path) == 8);
    assert(!pac_setting("margin") && !pac_setting("scale"));
    assert(!pac_setting("anchor") && !pac_setting("icons") && !pac_setting("print"));
    assert(!pac_setting("probe_host") && !pac_setting("usb"));
    assert(strcmp(pac_setting("font_size"), "22") == 0);      /* the old value stays */
    unlink(path);

    /* The size settings take a length with a unit, checked where it is read like the numbers are. */
    write_file(path, "margin = 5mm\nfont_size = 4MM\nwidth = 50%\nicon_size = 0.2 in\n");
    assert(settings_load_file(path) == 0);
    assert(strcmp(pac_setting("margin"), "5mm") == 0 && strcmp(pac_setting("icon_size"), "0.2 in") == 0);
    write_file(path, "margin = 5cm\nwidth = -3mm\nicon_size = 5mm5\nfont_size = 0mm\n");
    assert(settings_load_file(path) == 4);                        /* none of them is a length */
    assert(strcmp(pac_setting("margin"), "5mm") == 0);
    unlink(path);

    size_t n;
    const char *const *keys = settings_known(&n);
    for (size_t i = 0; i < n; i++)
        assert(conf_has_key("pacreact.conf", keys[i]));         /* every option is documented */
    puts("config ok");
    return 0;
}
