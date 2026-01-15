#define _POSIX_C_SOURCE 200112L
#include "net.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

void net_send_all(int fd, const char *s) {
    size_t len = strlen(s);
    while (len > 0) {
        ssize_t w = send(fd, s, len, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        s += w;
        len -= w;
    }
}

int net_make_listen_socket(const char *ip, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) die("socket");

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);

    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) die("bind");
    if (listen(s, 16) < 0) die("listen");
    return s;
}

void player_reset(Player *p) {
    if (!p) return;
    if (p->socket_fd >= 0) close(p->socket_fd);
    memset(p, 0, sizeof(*p));
    p->socket_fd = -1;
    p->current_room_id = -1;
    p->player_slot = -1;
    p->placing_mode = 0;
    p->pending_count = 0;
    p->last_ping = 0;
    p->hb_missed = 0;

    memset(p->pending, 0, sizeof(p->pending));
}

void player_soft_disconnect(Player *p) {
    if (!p) return;
    if (p->socket_fd >= 0) close(p->socket_fd);
    p->socket_fd = -1;
    p->connected = 0;
    p->placing_mode = 0;
    p->pending_count = 0;
    p->last_ping = 0;
    p->hb_missed = 0;

    memset(p->pending, 0, sizeof(p->pending));
}

void player_to_lobby(Player *p) {
    if (!p) return;
    p->current_room_id = -1;
    p->player_slot = -1;
    p->placing_mode = 0;
    p->pending_count = 0;
}

Player *find_player_by_fd(Player players[], int fd) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (players[i].socket_fd == fd) return &players[i];
    return NULL;
}
