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

typedef enum {
    HISTORY_SYNC_ALWAYS = 0,
    HISTORY_SYNC_NONE
} history_sync_mode_e;

static volatile sig_atomic_t stop_requested = 0;
static volatile sig_atomic_t reopen_requested = 0;

static unsigned long long events_received = 0;
static unsigned long long events_written = 0;
static unsigned long long events_dropped = 0;
static unsigned long long reopen_count = 0;

static void
handle_stop_signal(int signo)
{
    (void) signo;
    stop_requested = 1;
}

static void
handle_reopen_signal(int signo)
{
    (void) signo;
    reopen_requested = 1;
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

static int
open_history(const char *history_path)
{
    return open(history_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
}

static int
prepare_socket_path(const char *socket_path)
{
    struct stat st;

    if (lstat(socket_path, &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }

    if (!S_ISSOCK(st.st_mode)) {
        errno = EEXIST;
        return -1;
    }

    return unlink(socket_path);
}

static int
install_signal_handlers(void)
{
    struct sigaction stop_sa;
    struct sigaction reopen_sa;

    memset(&stop_sa, 0, sizeof(stop_sa));
    stop_sa.sa_handler = handle_stop_signal;
    sigemptyset(&stop_sa.sa_mask);

    memset(&reopen_sa, 0, sizeof(reopen_sa));
    reopen_sa.sa_handler = handle_reopen_signal;
    sigemptyset(&reopen_sa.sa_mask);

    if (sigaction(SIGINT, &stop_sa, NULL) != 0
        || sigaction(SIGTERM, &stop_sa, NULL) != 0
        || sigaction(SIGHUP, &reopen_sa, NULL) != 0)
    {
        return -1;
    }

    return 0;
}

static int
parse_sync_mode(const char *value, history_sync_mode_e *mode)
{
    if (strcmp(value, "always") == 0) {
        *mode = HISTORY_SYNC_ALWAYS;
        return 0;
    }

    if (strcmp(value, "none") == 0) {
        *mode = HISTORY_SYNC_NONE;
        return 0;
    }

    return -1;
}

static void
print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [--sync always|none] <socket-path> <history-jsonl>\n",
            program);
}

int
main(int argc, char **argv)
{
    const char *socket_path;
    const char *history_path;
    history_sync_mode_e sync_mode = HISTORY_SYNC_ALWAYS;
    struct sockaddr_un addr;
    int sock = -1;
    int history = -1;
    char event[HISTORY_EVENT_MAX];
    int argi = 1;
    int exit_code = 0;

    if (argc >= 3 && strcmp(argv[argi], "--sync") == 0) {
        if (argc < 5 || parse_sync_mode(argv[argi + 1], &sync_mode) != 0) {
            print_usage(argv[0]);
            return 2;
        }
        argi += 2;
    }

    if (argc - argi != 2) {
        print_usage(argv[0]);
        return 2;
    }

    socket_path = argv[argi];
    history_path = argv[argi + 1];

    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path is too long\n");
        return 2;
    }

    if (install_signal_handlers() != 0) {
        perror("sigaction");
        return 1;
    }

    history = open_history(history_path);
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

    if (prepare_socket_path(socket_path) != 0) {
        perror("prepare socket path");
        close(sock);
        close(history);
        return 1;
    }

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

    if (chmod(socket_path, 0600) != 0) {
        perror("chmod socket");
        close(sock);
        close(history);
        unlink(socket_path);
        return 1;
    }

    fprintf(stderr,
            "history collector listening on %s -> %s sync=%s pid=%ld\n",
            socket_path, history_path,
            sync_mode == HISTORY_SYNC_ALWAYS ? "always" : "none",
            (long) getpid());

    while (!stop_requested) {
        ssize_t n;

        if (reopen_requested) {
            int new_history;

            reopen_requested = 0;
            new_history = open_history(history_path);
            if (new_history < 0) {
                perror("reopen history");
                exit_code = 1;
                break;
            }

            close(history);
            history = new_history;
            reopen_count++;
            fprintf(stderr, "history file reopened after SIGHUP\n");
        }

        n = recv(sock, event, sizeof(event), 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            exit_code = 1;
            break;
        }

        if (n == 0) {
            continue;
        }

        events_received++;

        if ((size_t) n == sizeof(event)) {
            fprintf(stderr, "dropping oversized/truncated event\n");
            events_dropped++;
            continue;
        }

        if (event[0] != '{' || event[n - 1] != '}') {
            fprintf(stderr, "dropping malformed event\n");
            events_dropped++;
            continue;
        }

        if (write_all(history, event, (size_t) n) != 0
            || write_all(history, "\n", 1) != 0)
        {
            perror("write history");
            exit_code = 1;
            break;
        }

        if (sync_mode == HISTORY_SYNC_ALWAYS && fdatasync(history) != 0) {
            perror("fdatasync history");
            exit_code = 1;
            break;
        }

        events_written++;
    }

    fprintf(stderr,
            "history collector stopping received=%llu written=%llu dropped=%llu reopens=%llu\n",
            events_received, events_written, events_dropped, reopen_count);

    close(sock);
    close(history);
    unlink(socket_path);
    return exit_code;
}
