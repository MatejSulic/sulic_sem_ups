#include "game.h"
#include "log.h"
#include "net.h"
#include <stdio.h>
#include <string.h>

static int in_bounds(int x, int y) { return x >= 0 && x < GAME_N && y >= 0 && y < GAME_N; }

void game_reset(Game *g) {
    memset(g, 0, sizeof(*g));
    g->in_use = 0;
    g->room_id = -1;
}

void game_room_init(Game *g, int room_id) {
    memset(g, 0, sizeof(*g));
    g->in_use = 1;
    g->room_id = room_id;

    // Fleet definition (unique slots, includes two separate 3-len ships)
    g->ship_len[0] = 5;
    g->ship_len[1] = 4;
    g->ship_len[2] = 3;
    g->ship_len[3] = 3;
    g->ship_len[4] = 2;

    // init counts to len; if not placed yet, ships_left_count will be set when placed
    for (int p = 0; p < 2; p++) {
        g->ready[p] = 0;
        g->ships_alive[p] = 0;
        for (int s = 0; s < GAME_FLEET; s++) {
            g->ship_used[p][s] = 0;
            g->ships_left_count[p][s] = 0;
        }
    }

    g->turn = 0;
    g->finished = 0;
    g->winner = -1;
}

// NEW: clear only one player's setup (used by batch PLACING_STOP)
void game_clear_player_setup(Game *g, int slot) {
    if (!g || !g->in_use) return;
    if (slot < 0 || slot > 1) return;

    for (int y = 0; y < GAME_N; y++) {
        for (int x = 0; x < GAME_N; x++) {
            g->board[slot][y][x] = 0;
            g->ship_id[slot][y][x] = 0;
        }
    }

    g->ready[slot] = 0;
    g->ships_alive[slot] = 0;

    for (int s = 0; s < GAME_FLEET; s++) {
        g->ship_used[slot][s] = 0;
        g->ships_left_count[slot][s] = 0;
    }
}

int game_all_ready(const Game *g) {
    return g && g->ready[0] && g->ready[1];
}

static int find_free_ship_slot(Game *g, int player, int len) {
    for (int s = 0; s < GAME_FLEET; s++) {
        if (g->ship_len[s] == len && g->ship_used[player][s] == 0) return s;
    }
    return -1;
}

int game_place_ship(Game *g, int slot, int x, int y, int len, char dir, char *err, int errsz) {
    if (!g || !g->in_use) { snprintf(err, errsz, "NO_GAME"); return 0; }
    if (g->finished) { snprintf(err, errsz, "GAME_FINISHED"); return 0; }
    if (slot < 0 || slot > 1) { snprintf(err, errsz, "BAD_SLOT"); return 0; }
    if (g->ready[slot]) { snprintf(err, errsz, "ALREADY_READY"); return 0; }

    if (dir == 'h' || dir == 'H') dir = 'H';
    if (dir == 'v' || dir == 'V') dir = 'V';
    if (dir != 'H' && dir != 'V') { snprintf(err, errsz, "BAD_DIR"); return 0; }

    int ship_slot = find_free_ship_slot(g, slot, len);
    if (ship_slot < 0) { snprintf(err, errsz, "SHIP_NOT_AVAILABLE"); return 0; }

    // check bounds for all cells
    for (int i = 0; i < len; i++) {
        int cx = x + (dir == 'H' ? i : 0);
        int cy = y + (dir == 'V' ? i : 0);
        if (!in_bounds(cx, cy)) { snprintf(err, errsz, "OUT_OF_BOUNDS"); return 0; }
        if (g->board[slot][cy][cx] == 1) { snprintf(err, errsz, "OVERLAP"); return 0; }
    }

    // place
    unsigned char sid = (unsigned char)(ship_slot + 1);
    for (int i = 0; i < len; i++) {
        int cx = x + (dir == 'H' ? i : 0);
        int cy = y + (dir == 'V' ? i : 0);
        g->board[slot][cy][cx] = 1;
        g->ship_id[slot][cy][cx] = sid;
    }

    g->ship_used[slot][ship_slot] = 1;
    g->ships_left_count[slot][ship_slot] = len;
    g->ships_alive[slot] += len;

    snprintf(err, errsz, "OK");
    return 1;
}

static int fleet_complete(const Game *g, int slot) {
    for (int s = 0; s < GAME_FLEET; s++) {
        if (g->ship_used[slot][s] == 0) return 0;
    }
    return 1;
}

int game_set_ready(Game *g, int slot, char *err, int errsz) {
    if (!g || !g->in_use) { snprintf(err, errsz, "NO_GAME"); return 0; }
    if (slot < 0 || slot > 1) { snprintf(err, errsz, "BAD_SLOT"); return 0; }
    if (g->ready[slot]) { snprintf(err, errsz, "ALREADY_READY"); return 0; }
    if (!fleet_complete(g, slot)) { snprintf(err, errsz, "FLEET_INCOMPLETE"); return 0; }

    g->ready[slot] = 1;
    snprintf(err, errsz, "OK");
    return 1;
}

static int ship_is_sunk(const Game *g, int victim_slot, unsigned char sid) {
    int ship_slot = (int)sid - 1;
    if (ship_slot < 0 || ship_slot >= GAME_FLEET) return 0;
    return g->ships_left_count[victim_slot][ship_slot] == 0;
}

int game_shoot(Game *g, int slot, int x, int y, char *err, int errsz) {
    if (!g || !g->in_use) { snprintf(err, errsz, "NO_GAME"); return -1; }
    if (g->finished) { snprintf(err, errsz, "GAME_FINISHED"); return -1; }
    if (!game_all_ready(g)) { snprintf(err, errsz, "NOT_READY"); return -1; }
    if (slot != g->turn) { snprintf(err, errsz, "NOT_YOUR_TURN"); return -1; }
    if (!in_bounds(x, y)) { snprintf(err, errsz, "OUT_OF_BOUNDS"); return -1; }

    int enemy = (slot == 0) ? 1 : 0;
    unsigned char cell = g->board[enemy][y][x];

    if (cell == 2 || cell == 3) { snprintf(err, errsz, "ALREADY_SHOT"); return -1; }

    if (cell == 0) {
        g->board[enemy][y][x] = 3; // miss
        g->turn = enemy;
        snprintf(err, errsz, "OK");
        return 0; // water
    }

    // cell == 1
    g->board[enemy][y][x] = 2; // hit
    g->ships_alive[enemy] -= 1;

    unsigned char sid = g->ship_id[enemy][y][x];
    int ship_slot = (int)sid - 1;
    if (ship_slot >= 0 && ship_slot < GAME_FLEET) {
        if (g->ships_left_count[enemy][ship_slot] > 0) g->ships_left_count[enemy][ship_slot] -= 1;
    }

    if (g->ships_alive[enemy] == 0) {
        g->finished = 1;
        g->winner = slot;
        snprintf(err, errsz, "OK");
        return 3; // win
    }

    if (ship_is_sunk(g, enemy, sid)) {
        g->turn = enemy;
        snprintf(err, errsz, "OK");
        return 2; // sink
    }

    g->turn = enemy;
    snprintf(err, errsz, "OK");
    return 1; // hit
}

static void send_board_self(const Game *g, int slot, Player *to) {
    char line[128];
    for (int y = 0; y < GAME_N; y++) {
        char row[GAME_N + 1];
        for (int x = 0; x < GAME_N; x++) {
            unsigned char c = g->board[slot][y][x];
            row[x] = (c == 0) ? '.' : (c == 1) ? 'S' : (c == 2) ? 'H' : 'M';
        }
        row[GAME_N] = '\0';
        snprintf(line, sizeof(line), "BSELF %d %s\n", y, row);
        net_send_all(to->socket_fd, line);
    }
}

static void send_board_enemy_view(const Game *g, int slot, Player *to) {
    int enemy = (slot == 0) ? 1 : 0;
    char line[128];
    for (int y = 0; y < GAME_N; y++) {
        char row[GAME_N + 1];
        for (int x = 0; x < GAME_N; x++) {
            unsigned char c = g->board[enemy][y][x];
            row[x] = (c == 2) ? 'H' : (c == 3) ? 'M' : '.';
        }
        row[GAME_N] = '\0';
        snprintf(line, sizeof(line), "BENEMY %d %s\n", y, row);
        net_send_all(to->socket_fd, line);
    }
}

void game_send_state(const Game *g, const Room *r, Player *to) {
    if (!g || !r || !to || to->socket_fd < 0) return;

    char line[256];
    snprintf(line, sizeof(line), "PHASE %s\n", room_phase_str(r->phase));
    net_send_all(to->socket_fd, line);

    if (!g->in_use) {
        net_send_all(to->socket_fd, "STATE NO_GAME\n");
        return;
    }

    int slot = to->player_slot;
    if (slot < 0 || slot > 1) slot = 0;

    snprintf(line, sizeof(line), "STATE ROOM=%d YOU=%d READY=%d/%d TURN=%d FIN=%d WIN=%d\n",
             r->id, slot + 1, g->ready[0], g->ready[1], g->turn + 1,
             g->finished ? 1 : 0,
             g->finished ? (g->winner + 1) : 0);
    net_send_all(to->socket_fd, line);

    send_board_self(g, slot, to);
    send_board_enemy_view(g, slot, to);
}

void game_send_turn(const Game *g, const Room *r, Player players[]) {
    (void)players; // už ho nepotřebujeme

    if (!g || !r) return;
    if (!g->in_use || !game_all_ready(g) || g->finished) return;

    for (int slot = 0; slot < 2; slot++) {
        int fd = r->player_fds[slot];
        if (fd < 0) continue; // slot prázdný

        if (g->turn == slot) {
            net_send_all(fd, "YOUR_TURN\n");
            log_info("Turn -> slot=%d fd=%d YOUR_TURN", slot, fd);
        } else {
            net_send_all(fd, "OPP_TURN\n");
            log_info("Turn -> slot=%d fd=%d OPP_TURN", slot, fd);
        }
    }
}
