#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
    struct sockaddr_un addr;
    const char *socket_path;
    const char *payload;
    size_t payload_len;
    int sock;
    ssize_t sent;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <socket-path> <json-payload>\n", argv[0]);
        return 2;
    }

    socket_path = argv[1];
    payload = argv[2];
    payload_len = strlen(payload);

    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "socket path is too long\n");
        return 2;
    }

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, socket_path, strlen(socket_path) + 1);

    sent = sendto(sock, payload, payload_len, 0,
                  (struct sockaddr *) &addr,
                  (socklen_t) (offsetof(struct sockaddr_un, sun_path)
                               + strlen(socket_path) + 1));
    if (sent < 0) {
        perror("sendto");
        close(sock);
        return 1;
    }

    if ((size_t) sent != payload_len) {
        fprintf(stderr, "short datagram send: %zd of %zu bytes\n",
                sent, payload_len);
        close(sock);
        return 1;
    }

    close(sock);
    return 0;
}
