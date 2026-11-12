/* How long a notification card stays up, from its urgency and requested timeout. */
#include "../src/notify.h"
#include "../src/hud.h"
#include "../src/sound.h"
#include "../src/loop.h"
#include <assert.h>
#include <stdio.h>

void hud_card(const HudCard *c) { (void)c; }
void hud_dismiss(void) {}
bool hud_dismiss_id(uint32_t id) { (void)id; return false; }
const char *hud_action_key(int n, uint32_t *id) { (void)n; (void)id; return NULL; }
void hud_on_closed(HudClosedFn fn) { (void)fn; }
bool hud_remove(uint32_t id) { (void)id; return false; }
bool dnd_suppress(int urgency) { (void)urgency; return false; }
void sound_play(SoundKind kind) { (void)kind; }
void sound_play_spec(const char *s) { (void)s; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }
int loop_remove(int fd) { (void)fd; return 0; }

int main(void) {
    /* The server default (-1) depends on urgency: low, normal, critical */
    assert(notify_timeout_ms(0, -1) == 3000);
    assert(notify_timeout_ms(1, -1) == 6000);
    assert(notify_timeout_ms(2, -1) == 15000);
    assert(notify_timeout_ms(7, -1) == 15000);    /* nonsense urgency counts as critical */

    /* An explicit timeout is honoured, within bounds */
    assert(notify_timeout_ms(1, 8000) == 8000);
    assert(notify_timeout_ms(1, 100) == 1500);    /* too short to read */
    assert(notify_timeout_ms(1, 60000) == 30000); /* nothing can click a card away */
    assert(notify_timeout_ms(2, 2000) == 2000);   /* even critical ones obey a request */

    /* "Never expire" is capped; critical keeps its own default */
    assert(notify_timeout_ms(1, 0) == 30000);
    assert(notify_timeout_ms(0, 0) == 30000);
    assert(notify_timeout_ms(2, 0) == 15000);

    puts("notify: ok");
    return 0;
}
