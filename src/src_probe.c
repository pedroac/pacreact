#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sources.h"
#include <arpa/inet.h>
#include <netinet/ip_icmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* Opt-in: PACHUD_PROBE_HOST=<IPv4 address> sends one ICMP echo every PACHUD_PROBE_INTERVAL
 * seconds (default 30), and only while the network is up. It needs an unprivileged ping
 * socket (net.ipv4.ping_group_range). Nothing runs, and nothing wakes, without the variable. */

#define DEFAULT_INTERVAL_S 30
#define TIMEOUT_MS 2000
#define SLOW_MS 400
#define BAD_TO_ALERT 2    /* consecutive bad probes before alerting */
#define GOOD_TO_CLEAR 2   /* consecutive good probes before saying it recovered */

static char host_name[64] = "probe host";
static int bad, good;
static bool degraded;

/* Pure decision logic: one probe outcome in, at most one toast out. */
void probe_result(bool ok, int rtt_ms) {
    char msg[128];
    if (!ok || rtt_ms > SLOW_MS) {
        good = 0;
        if (++bad == BAD_TO_ALERT && !degraded) {
            degraded = true;
            if (ok) snprintf(msg, sizeof msg, _("Network slow — %d ms"), rtt_ms);
            else snprintf(msg, sizeof msg, _("Network not answering — no reply from %s"), host_name);
            hud_toast(msg, LEVEL_WARN, 6000);
        }
    } else {
        bad = 0;
        if (degraded && ++good >= GOOD_TO_CLEAR) {
            degraded = false;
            good = 0;
            hud_toast(_("Network back to normal"), LEVEL_INFO, 3000);
        }
    }
}

static int sock = -1, timer_fd = -1;
static struct sockaddr_in target;
static int interval_s = DEFAULT_INTERVAL_S;
static bool waiting;
static unsigned short seq;
static long long sent_ms;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void arm(long ms) {
    struct itimerspec its = { .it_value = { .tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000L } };
    timerfd_settime(timer_fd, 0, &its, NULL);
}

static void on_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(timer_fd, &n, sizeof n) < 0) return;

    if (waiting) {  /* no reply in time */
        waiting = false;
        probe_result(false, 0);
        arm(interval_s * 1000L);
        return;
    }
    if (!net_online()) { arm(interval_s * 1000L); return; }

    struct icmphdr echo = { .type = ICMP_ECHO, .code = 0 };
    echo.un.echo.sequence = ++seq;
    if (sendto(sock, &echo, sizeof echo, 0, (struct sockaddr *)&target, sizeof target) < 0) {
        arm(interval_s * 1000L);  /* e.g. network unreachable right now: the net source handles it */
        return;
    }
    waiting = true;
    sent_ms = now_ms();
    arm(TIMEOUT_MS);
}

static void on_reply(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    char buf[128];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof buf, MSG_DONTWAIT)) > 0) {
        struct icmphdr *h = (struct icmphdr *)buf;
        if ((size_t)n < sizeof *h || h->type != ICMP_ECHOREPLY || ntohs(h->un.echo.sequence) != seq)
            continue;
        if (!waiting) continue;
        waiting = false;
        probe_result(true, (int)(now_ms() - sent_ms));
        arm(interval_s * 1000L);
    }
}

void probe_init(void) {
    const char *host = getenv("PACHUD_PROBE_HOST"), *every = getenv("PACHUD_PROBE_INTERVAL");
    if (!host || !*host) return;
    target.sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &target.sin_addr) != 1) {
        fprintf(stderr, "pachud: PACHUD_PROBE_HOST must be an IPv4 address; probe disabled\n");
        return;
    }
    snprintf(host_name, sizeof host_name, "%s", host);
    if (every && atoi(every) >= 5 && atoi(every) <= 3600) interval_s = atoi(every);

    sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (sock < 0) {
        perror("pachud: probe disabled (ICMP socket; see net.ipv4.ping_group_range)");
        return;
    }
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) { close(sock); return; }
    loop_add(sock, EPOLLIN, on_reply, NULL);
    loop_add(timer_fd, EPOLLIN, on_timer, NULL);
    arm(interval_s * 1000L);
}
