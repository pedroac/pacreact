/* The shared helpers: strict whole numbers from settings, the data directory list, finding a tool. */
#include "../src/util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long num(const char *v) { return int_value("X", v, 7, 0, 100, "things"); }

int main(void) {
    /* Unset or empty: the default */
    assert(num(NULL) == 7 && num("") == 7);
    /* Inside the range, ends included */
    assert(num("0") == 0 && num("42") == 42 && num("100") == 100 && num("+5") == 5);
    assert(num(" 12") == 12);                                   /* leading space is forgiven */
    /* Not a whole number: the default */
    const char *bad[] = { "abc", "1.5", "10px", "5 ", "-", "0x10", "99999999999999999999999" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) assert(num(bad[i]) == 7);
    /* Outside the range: the nearest end */
    assert(num("101") == 100 && num("-1") == 0 && num("5000") == 100);
    /* The lower end need not be 0 */
    assert(int_value("X", "3", 50, 20, 150, "degrees") == 20 && int_value("X", "0", 50, 20, 150, "degrees") == 20);

    setenv("PACHUD_TEST_NUMBER", "33", 1);
    assert(env_int("PACHUD_TEST_NUMBER", 1, 0, 100, "things") == 33);
    unsetenv("PACHUD_TEST_NUMBER");
    assert(env_int("PACHUD_TEST_NUMBER", 1, 0, 100, "things") == 1);

    /* Data directories: the user's first, then the system's; defaults when unset */
    char list[1024];
    setenv("XDG_DATA_HOME", "/h", 1);
    setenv("XDG_DATA_DIRS", "/a:/b", 1);
    data_dirs(list, sizeof list);
    assert(!strcmp(list, "/h:/a:/b"));
    unsetenv("XDG_DATA_DIRS");
    setenv("HOME", "/home/x", 1);
    unsetenv("XDG_DATA_HOME");
    data_dirs(list, sizeof list);
    assert(!strcmp(list, "/home/x/.local/share:/usr/local/share:/usr/share"));

    /* Finding a tool: by path, or along PATH; regular executables only */
    char full[256];
    assert(find_in_path("/bin/sh", full, sizeof full) && !strcmp(full, "/bin/sh"));
    assert(!find_in_path("/etc/passwd", full, sizeof full));    /* a file, not executable */
    assert(!find_in_path("/usr", full, sizeof full));           /* a directory */
    setenv("PATH", "/nonexistent:/bin:/usr/bin", 1);
    assert(find_in_path("sh", full, sizeof full) && full[0] == '/');
    assert(!find_in_path("no-such-tool-here", full, sizeof full));

    puts("util: ok");
    return 0;
}
