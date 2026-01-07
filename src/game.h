#pragma once

#include "lobby.h"
#include "net.h"

// Simple Battleship-like rules:
// - board 10x10
// - ships: 5,4,3,3,2
// - no "touching" rules enforced (corners/sides allowed)

#define GAME_N 10
#define GAME_FLEET 5

typedef struct Game {
    int in_use;
    int room_id;

    // 0 empty, 1 ship, 2 hit, 3 miss
    unsigned char board[2][GAME_N][GAME_N];
    unsigned char ship_id[2][GAME_N][GAME_N];

    int ships_left_count[2][GAME_FLEET];   // remaining cells per ship
    int ships_alive[2];                    // remaining ship-cells total

    int ship_len[GAME_FLEET];
    int ship_used[2][GAME_FLEET];          // how many of given slot already placed (for 3 appears twice we represent as two separate entries)

    int ready[2];
    int turn;          // 0/1
    int finished;      // 0/1
    int winner;        // 0/1 if finished
} Game;

void game_reset(Game *g);
void game_room_init(Game *g, int room_id);

int  game_all_ready(const Game *g);

// returns 1 on success, 0 on error
int  game_place_ship(Game *g, int slot, int x, int y, int len, char dir, char *err, int errsz);
int  game_set_ready(Game *g, int slot, char *err, int errsz);

// shoot result: 0 water, 1 hit, 2 sink, 3 win
int  game_shoot(Game *g, int slot, int x, int y, char *err, int errsz);

void game_send_state(const Game *g, const Room *r, Player *to);
void game_send_turn(const Game *g, const Room *r, Player players[]);
