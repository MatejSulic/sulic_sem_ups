#pragma once

#include "common.h"
#include <stddef.h>
#include <time.h>

#define PENDING_MAX 5

typedef struct PendingShip {
    int x;
    int y;
    int len;
    char dir; // 'H' or 'V'
} PendingShip;

typedef struct Player {
    int socket_fd;

    char rx_buffer[BUF_SIZE];
    size_t rx_len;

    int is_identified;
    char player_name[32];

    int current_room_id;
    int player_slot;

    int invalid_count;

    int connected;
    time_t disconnected_at;

    // === batch setup placement ===
    int placing_mode;
    int pending_count;
    PendingShip pending[PENDING_MAX];

        // === heartbeat (server -> client PING, client -> server PONG) ===
    time_t last_ping;   // last time server sent PING
    int hb_missed;      // consecutive missed PONGs


} Player;

int net_make_listen_socket(const char *ip, int port);
void net_send_all(int fd, const char *s);

void player_reset(Player *p);
void player_soft_disconnect(Player *p);
void player_to_lobby(Player *p);

Player *find_player_by_fd(Player players[], int fd);
