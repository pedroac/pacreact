#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sources.h"
#include "sound.h"
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define DEBOUNCE_MS 1000  /* link flaps and resume bursts settle before we judge */

static int timer_fd = -1;
static int last_dev = -1;  /* ifindex of the default route's device, 0 when offline */

/* Returns the interface of the main-table default route (IPv4 or IPv6), or 0 if none. */
static int default_route_dev(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;

    struct { struct nlmsghdr nh; struct rtmsg rt; } req = {
        .nh = { .nlmsg_len = sizeof req, .nlmsg_type = RTM_GETROUTE,
                .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP },
        .rt = { .rtm_family = AF_UNSPEC },
    };
    int dev = 0;
    if (send(fd, &req, sizeof req, 0) < 0) { close(fd); return -1; }

    char buf[16384];
    for (bool done = false; !done;) {
        ssize_t len = recv(fd, buf, sizeof buf, 0);
        if (len <= 0) break;
        for (struct nlmsghdr *nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, (size_t)len);
             nh = NLMSG_NEXT(nh, len)) {
            if (nh->nlmsg_type == NLMSG_DONE || nh->nlmsg_type == NLMSG_ERROR) { done = true; break; }
            if (nh->nlmsg_type != RTM_NEWROUTE) continue;
            struct rtmsg *rt = NLMSG_DATA(nh);
            if (rt->rtm_dst_len != 0 || rt->rtm_table != RT_TABLE_MAIN || rt->rtm_type != RTN_UNICAST)
                continue;
            int rlen = RTM_PAYLOAD(nh);
            for (struct rtattr *a = RTM_RTA(rt); RTA_OK(a, rlen); a = RTA_NEXT(a, rlen))
                if (a->rta_type == RTA_OIF) dev = *(int *)RTA_DATA(a);
            if (!dev) dev = -2;  /* a default route without an interface (multipath): still online */
        }
    }
    close(fd);
    return dev == -2 ? 1 : dev;
}

bool net_online(void) { return last_dev > 0; }

static void on_event(void *ctx, uint32_t ev) {
    (void)ev;
    char buf[8192];
    while (recv(*(int *)ctx, buf, sizeof buf, MSG_DONTWAIT) > 0) {}
    struct itimerspec its = { .it_value = { .tv_sec = DEBOUNCE_MS / 1000,
                                            .tv_nsec = (DEBOUNCE_MS % 1000) * 1000000L } };
    timerfd_settime(timer_fd, 0, &its, NULL);  /* re-arming restarts the debounce */
}

static void on_settled(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(timer_fd, &n, sizeof n) < 0) return;

    int dev = default_route_dev();
    if (dev < 0) return;
    bool was_online = last_dev > 0, online = dev > 0;
    last_dev = dev;
    if (was_online == online) return;

    if (!online) {
        hud_toast_icon(_("Offline"), LEVEL_WARN, 4000, "network-offline,network-error");
        sound_play(SOUND_NETWORK_DOWN);
    } else {
        char name[64] = "", msg[128];
        if (!if_indextoname(dev, name)) snprintf(name, sizeof name, "%s", _("network"));
        snprintf(msg, sizeof msg, _("Online — %s"), name);
        /* wlan0, wlp3s0: a wireless interface by its name; anything else is taken to be a cable */
        hud_toast_icon(msg, LEVEL_INFO, 3000,
                       name[0] == 'w' && name[1] == 'l' ? "network-wireless,network-wired" : "network-wired,network-idle");
        sound_play(SOUND_NETWORK_UP);
    }
}

void net_init(void) {
    static int fd;
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
    if (fd < 0) return;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK,
                              .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE };
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return; }

    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) { close(fd); return; }
    last_dev = default_route_dev();  /* initial state, reported silently */
    loop_add(fd, EPOLLIN, on_event, &fd);
    loop_add(timer_fd, EPOLLIN, on_settled, NULL);
}
