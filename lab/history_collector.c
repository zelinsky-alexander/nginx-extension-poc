#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define HISTORY_EVENT_MAX 4096

static volatile sig_atomic_t stop_requested = 0;

static void
handle_signal(int signo)
{
    (void) signo;
    stop_requested = 1;
}

static int
write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n > 0) {
            buf += (size_t) n;
            len -= (size_t) n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

int
main(int argc, char **argv)
{
    const char *socket_path;
    const char *history_path;
    struct sockaddr_un addr;
    struct sigaction sa;
    int sock = -1;
    int history = -1;
    char event[HISTORY_EVENT_MAX];

    if (argc != 3) {
        fprintf(stderr, "usage: %s <socket-path> <history-jsonl>\n", argv[0]);
        return 2;
    }

    socket_path = argv[1];
    history_path = argv[2];

    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path is too long\n");
        return 2;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0
        || sigaction(SIGTERM, &sa, NULL) != 0)
    {
        perror("sigaction");
        return 1;
    }

    history = open(history_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (history < 0) {
        perror("open history");
        return 1;
    }

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        close(history);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, socket_path, strlen(socket_path) + 1);

    unlink(socket_path);
    umask(0077);

    if (bind(sock, (struct sockaddr *) &addr,
             (socklen_t) (offsetof(struct sockaddr_un, sun_path)
                          + strlen(socket_path) + 1)) != 0)
    {
        perror("bind");
        close(sock);
        close(history);
        return 1;
    }

    fprintf(stderr, "history collector listening on %s -> %s\n",
            socket_path, history_path);

    while (!stop_requested) {
        ssize_t n = recv(sock, event, sizeof(event), 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            break;
        }

        if (n == 0) {
            continue;
        }

        if ((size_t) n == sizeof(event)) {
            fprintf(stderr, "dropping oversized/truncated event\n");
            continue;
        }

        if (event[0] != '{' || event[n - 1] != '}') {
            fprintf(stderr, "dropping malformed event\n");
            continue;
        }

        if (write_all(history, event, (size_t) n) != 0
            || write_all(history, "\n", 1) != 0)
        {
            perror("write history");
            break;
        }

        if (fdatasync(history) != 0) {
            perror("fdatasync history");
            break;
        }
    }

    close(sock);
    close(history);
    unlink(socket_path);
    return 0;
}
