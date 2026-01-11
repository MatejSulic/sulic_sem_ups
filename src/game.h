#pragma once

#include "lobby.h"
#include "net.h"

#define GAME_N 10
#define GAME_FLEET 5

typedef struct Game {
    int in_use;
    int room_id;

    unsigned char board[2][GAME_N][GAME_N];
    unsigned char ship_id[2][GAME_N][GAME_N];

    int ship_len[GAME_FLEET];
    int ship_used[2][GAME_FLEET];
    int ships_left_count[2][GAME_FLEET];
    int ships_alive[2];

    int ready[2];
    int turn;
    int finished;
    int winner;
} Game;

void game_reset(Game *g);
void game_room_init(Game *g, int room_id);
void game_clear_player_setup(Game *g, int slot);

int game_all_ready(const Game *g);
int game_place_ship(Game *g, int slot, int x, int y, int len, char dir, char *err, int errsz);
int game_set_ready(Game *g, int slot, char *err, int errsz);
int game_shoot(Game *g, int slot, int x, int y, char *err, int errsz);

void game_send_state(const Game *g, const Room *r, Player *to);
void game_send_turn(const Game *g, const Room *r, Player players[]);
