#pragma once

#include "common.h"

#include <stddef.h>
#include <time.h>

typedef struct Player {
    int socket_fd;

    char rx_buffer[BUF_SIZE];
    size_t rx_len;

    int is_identified;
    char player_name[32];

    int current_room_id; // -1 if none
    int player_slot;     // 0/1 in room, -1 if none

    int invalid_count;

    // reconnect support
    int connected;              // 1=socket alive, 0=soft-disconnected
    time_t disconnected_at;     // when it went down
} Player;

int net_make_listen_socket(const char *ip, int port);
void net_send_all(int fd, const char *s);

void player_reset(Player *p);
void player_soft_disconnect(Player *p);

Player* find_player_by_fd(Player players[], int fd);
