#define _POSIX_C_SOURCE 200112L
#include "net.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void die(const char *msg) { perror(msg); exit(1); }

void net_send_all(int fd, const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = send(fd, s, n, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += (size_t)w;
        n -= (size_t)w;
    }
}

int net_make_listen_socket(const char *ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) die("socket");

    int yes = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
        die("setsockopt(SO_REUSEADDR)");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Bad IP: %s\n", ip);
        exit(1);
    }

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(s, 16) < 0) die("listen");
    return s;
}

void player_soft_disconnect(Player *p) {
    if (!p) return;
    if (p->socket_fd >= 0) close(p->socket_fd);
    p->socket_fd = -1;
    p->rx_len = 0;
    p->connected = 0;
    p->disconnected_at = time(NULL);
}

void player_reset(Player *p) {
    if (!p) return;
    if (p->socket_fd >= 0) close(p->socket_fd);
    p->socket_fd = -1;
    p->rx_len = 0;
    p->is_identified = 0;
    p->player_name[0] = '\0';
    p->current_room_id = -1;
    p->player_slot = -1;
    p->invalid_count = 0;
    p->connected = 0;
    p->disconnected_at = 0;
}

Player* find_player_by_fd(Player players[], int fd) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (players[i].socket_fd == fd) return &players[i];
    return NULL;
}
