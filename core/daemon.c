// Copyright (c) 2015, Louis Opter <kalessin@kalessin.fr>
//
// This file is part of lighstd.
//
// lighstd is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// lighstd is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with lighstd.  If not, see <http://www.gnu.org/licenses/>.

#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/tree.h>
#include <sys/types.h>
#include <sys/un.h>
#include <assert.h>
#include <endian.h>
#include <err.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <event2/util.h>

#include "time_monotonic.h"
#include "lifx/wire_proto.h"
#include "lifx/bulb.h"
#include "lifx/gateway.h"
#include "jsmn.h"
#include "jsonrpc.h"
#include "client.h"
#include "listen.h"
#include "daemon.h"
#include "pipe.h"
#include "stats.h"
#include "lightsd.h"

static bool lgtd_daemon_proctitle_initialized = false;

bool
lgtd_daemon_unleash(void)
{
    if (chdir("/")) {
        return false;
    }

    int null = open("/dev/null", O_RDWR);
    if (null == -1) {
        return false;
    }

    const int fds[] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    for (int i = 0; i != LGTD_ARRAY_SIZE(fds); ++i) {
        if (dup2(null, fds[i]) == -1) {
            close(null);
            return false;
        }
    }
    close(null);

#define SUMMON()  do {      \
    switch (fork()) {       \
        case 0:             \
            break;          \
        case -1:            \
            return false;   \
        default:            \
            exit(0);        \
    }                       \
} while (0)

    SUMMON(); // \_o< !
    setsid();

    SUMMON(); // \_o< !!

    return true; // welcome to UNIX!
}

void
lgtd_daemon_setup_proctitle(int argc, char *argv[], char *envp[])
{
#if LGTD_HAVE_SETPROCTITLE
    (void)argc;
    (void)argv;
    (void)envp;
#else
    void setproctitle_init(int argc, char *argv[], char *envp[]);

    setproctitle_init(argc, argv, envp);
    lgtd_daemon_update_proctitle();
    lgtd_daemon_proctitle_initialized = true;
#endif
}

static char *
lgtd_daemon_update_proctitle_format_sockaddr(const struct sockaddr *sa,
                                             char *buf,
                                             int buflen)
{
    assert(sa);
    assert(buf);
    assert(buflen > 0);

    if (sa->sa_family == AF_UNIX) {
        return ((struct sockaddr_un *)sa)->sun_path;
    }

    return lgtd_sockaddrtoa(sa, buf, buflen);
}

void
lgtd_daemon_update_proctitle(void)
{
    if (!lgtd_daemon_proctitle_initialized) {
        return;
    }

#if !LGTD_HAVE_SETPROCTITLE
    void setproctitle(const char *fmt, ...);
#endif

    char title[LGTD_DAEMON_TITLE_SIZE] = { 0 };
    int i = 0;

#define TITLE_APPEND(fmt, ...) LGTD_SNPRINTF_APPEND(    \
    title, i, (int)sizeof(title), (fmt), __VA_ARGS__    \
)

#define PREFIX(fmt, ...) TITLE_APPEND(                              \
    "%s" fmt, (i && title[i - 1] == ')' ? "; " : ""), __VA_ARGS__   \
)

#define ADD_ITEM(fmt, ...) TITLE_APPEND(                            \
    "%s" fmt, (i && title[i - 1] != '(' ? ", " : ""), __VA_ARGS__   \
)
#define LOOP(list_type, list, elem_type, prefix, ...) do {    \
    if (!list_type ## _EMPTY(list)) {                         \
        PREFIX("%s(", prefix);                                \
        elem_type *it;                                        \
        list_type ## _FOREACH(it, list, link) {               \
            ADD_ITEM(__VA_ARGS__);                            \
        }                                                     \
        TITLE_APPEND("%s", ")");                              \
    }                                                         \
} while (0)

    char addr[LGTD_SOCKADDR_STRLEN];
    LOOP(
        SLIST, &lgtd_listeners, struct lgtd_listen,
        "listening_on", "%s",
        lgtd_daemon_update_proctitle_format_sockaddr(
            it->sockaddr, addr, sizeof(addr)
        )
    );

    LOOP(
        SLIST, &lgtd_command_pipes, struct lgtd_command_pipe,
        "command_pipes", "%s", it->path
    );

    if (!LIST_EMPTY(&lgtd_lifx_gateways)) {
        PREFIX("lifx_gateways(found=%d)", LGTD_STATS_GET(gateways));
    }

    PREFIX(
        "bulbs(found=%d, on=%d)",
        LGTD_STATS_GET(bulbs), LGTD_STATS_GET(bulbs_powered_on)
    );

    PREFIX("clients(connected=%d)", LGTD_STATS_GET(clients));

    setproctitle("%s", title);
}

void
lgtd_daemon_die_if_running_as_root_unless_requested(const char *requested_user)
{
    if (requested_user && !strcmp(requested_user, "root")) {
        return;
    }

    if (geteuid() == 0 || getegid() == 0) {
        lgtd_errx(
            1,
            "not running as root unless -u root is passed in; if you don't "
            "understand why this very basic safety measure is in place and "
            "use -u root then you deserve to be thrown under a bus, kthx bye."
        );
    }
}

void
lgtd_daemon_drop_privileges(const char *user, const char *group)
{
    assert(user);

    uid_t uid;
    gid_t gid;

    struct passwd *user_info = getpwnam(user);
    if (!user_info) {
        lgtd_err(1, "can't get user info for %s", user);
    }
    uid = user_info->pw_uid;

    struct group *group_info;
    if (group) {
        group_info = getgrnam(group);
    } else {
        group_info = getgrgid(user_info->pw_gid);
        group = group_info->gr_name;
    }
    if (!group_info) {
        lgtd_err(1, "can't get group info for %s", group ? group : user);
    }
    gid = group_info->gr_gid;

    struct lgtd_command_pipe *pipe;
    SLIST_FOREACH(pipe, &lgtd_command_pipes, link) {
        if (fchown(pipe->fd, uid, gid) == -1) {
            lgtd_err(1, "can't chown %s to %s:%s", pipe->path, user, group);
        }
    }

    struct lgtd_listen *listener;
    SLIST_FOREACH(listener, &lgtd_listeners, link) {
        if (listener->sockaddr->sa_family != AF_UNIX) {
            continue;
        }

        const char *path = ((struct sockaddr_un *)listener->sockaddr)->sun_path;
        if (chown(path, uid, gid) == -1) {
            char addr[LGTD_SOCKADDR_STRLEN];
            lgtd_err(
                1, "can't chown %s to %s:%s",
                LGTD_SOCKADDRTOA(listener->sockaddr, addr), user, group
            );
        }
    }

    if (setgid(gid) == -1) {
        lgtd_err(1, "can't change group to %s", group);
    }

    if (setgroups(1, &gid) == -1) {
        lgtd_err(1, "can't change group to %s", group);
    }

    if (setuid(uid) == -1) {
        lgtd_err(1, "can't change user to %s", user);
    }
}
