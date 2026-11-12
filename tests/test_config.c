/* The settings file: keys become PACHUD_ variables, the environment wins, bad lines are skipped. */
#include "../src/config_file.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[] = "/tmp/pachud-conf-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    const char *text =
        "# a comment\n"
        "\n"
        "font_size = 22   # trailing comment\n"
        "Peek-Idle=7\n"
        "font = /usr/share/fonts/My Font.ttf\n"
        "width = 400\n"
        "no equals sign\n"
        "bogus = 1\n"
        "sound =\n"
        "PATH = /nowhere\n";
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    close(fd);

    unsetenv("PACHUD_FONT_SIZE"); unsetenv("PACHUD_PEEK_IDLE"); unsetenv("PACHUD_FONT");
    unsetenv("PACHUD_SOUND"); unsetenv("PACHUD_BOGUS");
    setenv("PACHUD_WIDTH", "300", 1);                 /* the environment beats the file */
    char *old_path = strdup(getenv("PATH"));

    assert(config_load_file(path) == 3);              /* no '=', bogus, PATH */
    assert(strcmp(getenv("PACHUD_FONT_SIZE"), "22") == 0);
    assert(strcmp(getenv("PACHUD_PEEK_IDLE"), "7") == 0);          /* case and '-' forgiven */
    assert(strcmp(getenv("PACHUD_FONT"), "/usr/share/fonts/My Font.ttf") == 0);
    assert(strcmp(getenv("PACHUD_WIDTH"), "300") == 0);
    assert(strcmp(getenv("PACHUD_SOUND"), "") == 0);                /* an empty value is a value */
    assert(!getenv("PACHUD_BOGUS"));
    assert(strcmp(getenv("PATH"), old_path) == 0);    /* only pachud's own settings */

    unlink(path);
    assert(config_load_file(path) == -1);             /* no file: nothing happens */
    puts("config ok");
    return 0;
}
