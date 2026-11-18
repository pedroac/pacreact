/* Finding a card's icon: paths, theme names across theme layouts, precedence, and refusing
 * anything that could leave the icon directories. */
#include "../src/icon.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[64];

static void touch(const char *rel) {
    char path[512], cmd[600];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname %s)\"", path);
    assert(system(cmd) == 0);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    assert(fd >= 0);
    close(fd);
}

static bool find(const char *spec, char *out) { return icon_find(spec, out, 512); }

/* ---- status icons ---- */

static bool status(const char *names, bool svg, char *out, bool *symbolic) {
    return icon_find_status(names, svg, out, 512, symbolic);
}

static void test_status(void) {
    char out[512];
    bool sym = false;
    snprintf(out, sizeof out, "%s/gtk-3.0/settings.ini", root);   /* whatever theme the checks before chose */
    unlink(out);
    /* The Adwaita layout: symbolic/CONTEXT and scalable/CONTEXT, SVG */
    touch("data/icons/Adwaita/symbolic/status/battery-low-symbolic.svg");
    touch("data/icons/Adwaita/scalable/status/battery-low-symbolic.svg");
    touch("data/icons/Adwaita/symbolic/devices/audio-headset-symbolic.svg");
    touch("data/icons/Adwaita/scalable/status/only-coloured.svg");
    /* The Breeze layout: CONTEXT/SIZE, SVG */
    touch("data/icons/breeze/status/22/network-offline.svg");
    touch("data/icons/breeze/status/16/network-offline.svg");
    /* The legacy raster layout: SIZExSIZE/CONTEXT, PNG */
    touch("data/icons/AdwaitaLegacy/24x24/legacy/audio-volume-high.png");
    touch("data/icons/AdwaitaLegacy/16x16/status/dialog-warning.png");

    /* With an SVG renderer: the symbolic icon, in the HUD's own colour, in the contexts themes use */
    assert(status("battery-low", true, out, &sym) && strstr(out, "/Adwaita/symbolic/status/battery-low-symbolic.svg") && sym);
    assert(status("audio-headset", true, out, &sym) && strstr(out, "/symbolic/devices/") && sym);
    assert(status("only-coloured", true, out, &sym) && strstr(out, "/scalable/status/only-coloured.svg") && !sym);

    /* Without one: SVG files are not even looked at, and only a PNG will do */
    assert(!status("battery-low", false, out, &sym));
    assert(!status("only-coloured", false, out, &sym));
    assert(status("audio-volume-high", false, out, &sym) && strstr(out, "/AdwaitaLegacy/24x24/legacy/") && !sym);
    assert(status("dialog-warning", false, out, &sym) && strstr(out, "/16x16/status/"));
    assert(status("audio-volume-high", true, out, &sym) && strstr(out, ".png"));   /* a PNG is fine for both */

    /* Breeze is only looked in when it is the configured theme: until then, the icon is not found */
    assert(!status("network-offline", true, out, &sym));
    char cfg[160];
    snprintf(cfg, sizeof cfg, "%s/gtk-3.0", root);
    snprintf(out, sizeof out, "mkdir -p %s", cfg);
    assert(system(out) == 0);
    snprintf(out, sizeof out, "%s/settings.ini", cfg);
    FILE *f = fopen(out, "w");
    assert(f && fputs("[Settings]\ngtk-icon-theme-name=breeze\n", f) >= 0 && !fclose(f));
    assert(status("network-offline", true, out, &sym) && strstr(out, "/breeze/status/22/network-offline.svg") && !sym);

    /* The configured theme comes first, even when another has a better kind of icon */
    touch("data/icons/breeze/status/22/battery-low.svg");
    assert(status("battery-low", true, out, &sym) && strstr(out, "/breeze/status/22/") && !sym);

    /* Several names, comma separated: the first that exists, in the order written */
    assert(status("no-such-icon,audio-headset", true, out, &sym) && strstr(out, "audio-headset"));
    assert(status("audio-headset,battery-low", true, out, &sym) && strstr(out, "audio-headset"));
    assert(status("battery-low,audio-headset", true, out, &sym) && strstr(out, "battery-low"));
    assert(!status("no-such-icon,also-missing", true, out, &sym));

    /* Anything that could leave the icon directories, or is no name at all, is refused */
    assert(!status("../../etc/passwd", true, out, &sym));
    assert(!status("/etc/passwd", true, out, &sym));
    assert(!status(".hidden", true, out, &sym));
    assert(!status("a/b", true, out, &sym));
    assert(!status("", true, out, &sym) && !status(",,", true, out, &sym) && !status(NULL, true, out, &sym));
    char long_name[200];
    memset(long_name, 'a', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    assert(!status(long_name, true, out, &sym));
    assert(status("no-such,../x,battery-low", true, out, &sym) && strstr(out, "battery-low"));   /* a bad one is skipped */

    snprintf(out, sizeof out, "%s/settings.ini", cfg);
    unlink(out);
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/pacreact-icon-XXXXXX");
    assert(mkdtemp(root));
    char data[128], home[128], out[512], expect[512];
    snprintf(data, sizeof data, "%s/data", root);
    snprintf(home, sizeof home, "%s/home", root);
    setenv("XDG_DATA_DIRS", data, 1);
    setenv("XDG_DATA_HOME", home, 1);
    setenv("XDG_CONFIG_HOME", root, 1);          /* settings.ini lives under here */

    touch("data/icons/hicolor/48x48/apps/firefox.png");
    touch("data/icons/hicolor/256x256/apps/big-only.png");
    touch("data/icons/hicolor/16x16/apps/small-first.png");
    touch("data/icons/hicolor/48x48/status/dialog-information.png");
    touch("data/icons/Adwaita/32x32/devices/audio-card.png");
    touch("data/icons/breeze/apps/48/breeze-layout.png");        /* the CONTEXT/SIZE layout */
    touch("data/pixmaps/legacy.png");
    touch("home/icons/hicolor/48x48/apps/firefox.png");           /* the user's own beats the system's */
    touch("data/icons/hicolor/48x48/apps/only-svg.svg");          /* SVG alone is not usable */

    /* Theme names */
    assert(find("firefox", out));
    snprintf(expect, sizeof expect, "%s/icons/hicolor/48x48/apps/firefox.png", home);
    assert(!strcmp(out, expect));
    assert(find("dialog-information", out) && strstr(out, "/hicolor/48x48/status/"));
    assert(find("audio-card", out) && strstr(out, "/Adwaita/32x32/devices/"));
    assert(find("big-only", out) && strstr(out, "/256x256/"));
    assert(find("small-first", out) && strstr(out, "/16x16/"));
    assert(find("legacy", out) && strstr(out, "/pixmaps/legacy.png"));
    assert(!find("only-svg", out));
    assert(!find("no-such-icon", out));

    /* A configured GTK theme is tried before hicolor, including the other directory layout */
    touch("gtk-3.0/settings.ini");
    char ini[512];
    snprintf(ini, sizeof ini, "%s/gtk-3.0/settings.ini", root);
    FILE *f = fopen(ini, "w");
    fputs("[Settings]\ngtk-theme-name=Foo\ngtk-icon-theme-name=breeze\n", f);
    fclose(f);
    assert(find("breeze-layout", out) && strstr(out, "/breeze/apps/48/"));

    /* Paths: must exist; file:// is accepted */
    snprintf(expect, sizeof expect, "%s/data/pixmaps/legacy.png", root);
    assert(find(expect, out) && !strcmp(out, expect));
    char url[600];
    snprintf(url, sizeof url, "file://%s", expect);
    assert(find(url, out) && !strcmp(out, expect));
    assert(!find("/no/such/icon.png", out));

    /* Nothing that could wander out of the icon directories, or look like an option */
    assert(!find("../../etc/passwd", out));
    assert(!find("../../../../pixmaps/legacy", out));           /* a real file, reached by walking up */
    assert(!find("sub/dir", out));
    assert(!find(".hidden", out));
    assert(!find("", out) && !icon_find(NULL, out, sizeof out));
    char longname[300];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    assert(!find(longname, out));

    /* An icon that appears later is found (misses are not remembered)... */
    assert(!find("late", out));
    touch("data/icons/hicolor/48x48/apps/late.png");
    assert(find("late", out));
    /* ...and one that disappears is not served stale from the cache */
    snprintf(expect, sizeof expect, "%s/data/icons/hicolor/48x48/apps/late.png", root);
    assert(unlink(expect) == 0);
    assert(!find("late", out));

    test_status();

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
    puts("icon: ok");
    return 0;
}
