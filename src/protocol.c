#define _POSIX_C_SOURCE 200112L
#include "protocol.h"
#include "net.h"
#include "lobby.h"
#include "game.h"
#include "log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define MAX_INVALID 5
// batch placement helpers
static void pending_reset(Player *p) {
    if (!p) return;
    p->placing_mode = 0;
    p->pending_count = 0;
    memset(p->pending, 0, sizeof(p->pending));
}


// forward (used by close_room_now)
static Game* game_for_room(Room *r, Game games[]);

static void close_room_now(Room *r, Game games[], Player players[], const char *reason) {
    if (!r) return;

    char msg[128];
    if (!reason) reason = "CLOSED";
    snprintf(msg, sizeof(msg), "ROOM_CLOSED %s\n", reason);

    // reset game
    Game *g = game_for_room(r, games);
    if (g) game_reset(g);

    // Detach players bound to this room.
    // CONNECTED players: keep socket, just move to lobby.
    // DISCONNECTED placeholders (for rejoin): free the slot.
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *pp = &players[i];
        if (!pp->is_identified) continue;
        if (pp->current_room_id != r->id) continue;

        if (pp->socket_fd >= 0 && pp->connected) {
            // still connected -> do NOT close fd
            net_send_all(pp->socket_fd, msg);
            net_send_all(pp->socket_fd, "RETURNED_TO_LOBBY\n");

            pp->current_room_id = -1;
            pp->player_slot = -1;
            pp->invalid_count = 0;
            pp->connected = 1;
            pp->rx_len = 0;
        } else {
            // ghost slot reserved for rejoin (socket_fd == -1) or marked disconnected
            // room is gone -> free it
            player_reset(pp);
        }
    }

    room_reset(r);
}

static void strike(Player *p, Room rooms[], Game games[], Player players[], const char *msg) {
    if (!p) return;
    p->invalid_count++;
    if (msg && p->socket_fd >= 0) net_send_all(p->socket_fd, msg);

    if (p->invalid_count >= MAX_INVALID) {
        if (p->socket_fd >= 0) net_send_all(p->socket_fd, "ERROR TOO_MANY_ERRORS\n");
        log_warn("fd=%d too many errors -> disconnect", p->socket_fd);

        // Protocol abuse: close the room immediately if the player was in one.
        if (p->current_room_id != -1 && rooms && games && players) {
            Room *r = find_room_by_id(rooms, p->current_room_id);
            if (r) close_room_now(r, games, players, "PROTOCOL");
        }

        // Always hard reset this player's slot (closes socket)
        player_reset(p);
    }
}

static void room_set_phase(Room *r, RoomPhase ph) {
    if (!r) return;
    if (r->phase == ph) return;
    log_info("room=%d phase %s -> %s", r->id, room_phase_str(r->phase), room_phase_str(ph));
    r->phase = ph;
}

static Game* game_for_room(Room *r, Game games[]) {
    if (!r) return NULL;
    int idx = r->id - 1;
    if (idx < 0 || idx >= MAX_ROOMS) return NULL;
    return &games[idx];
}

static void notify_opponent(Room *r, Player players[], int slot, const char *msg) {
    (void)players; 
    if (!r || !msg) return;

    int opp = (slot == 0) ? 1 : 0;

    int fd = r->player_fds[opp];
    if (fd < 0) return;                 

    net_send_all(fd, msg);
}


static Player* find_disconnected_player_by_nick(Player players[], const char *nick, int room_id, int slot) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &players[i];
        if (p->socket_fd == -1 && p->is_identified && p->current_room_id == room_id && p->player_slot == slot) {
            if (strcmp(p->player_name, nick) == 0) return p;
        }
    }
    return NULL;
}

// --- command handlers ---

static void cmd_hello(Player *p, const char *name) {
    if (p->is_identified) { net_send_all(p->socket_fd, "ERROR ALREADY_HELLO\n"); return; }
    if (!name || name[0] == '\0') { net_send_all(p->socket_fd, "ERROR BAD_ARGS\n"); strike(p, NULL, NULL, NULL, NULL); return; }

    snprintf(p->player_name, sizeof(p->player_name), "%s", name);
    size_t L = strlen(p->player_name);
    if (L > 0 && p->player_name[L-1] == '\r') p->player_name[L-1] = '\0';

    p->is_identified = 1;
    p->connected = 1;

    char out[128];
    snprintf(out, sizeof(out), "WELCOME %s\n", p->player_name);
    net_send_all(p->socket_fd, out);

    log_info("player fd=%d identified as '%s'", p->socket_fd, p->player_name);
}

static void cmd_list(Player *p, Room rooms[], Game games[], Player players[]) {
    (void)games; (void)players;
    if (!p->is_identified) { net_send_all(p->socket_fd, "ERROR MUST_HELLO\n"); strike(p, rooms, games, players, NULL); return; }
    lobby_send_room_list(p->socket_fd, rooms);
}

static void cmd_create(Player *p, Room rooms[], Game games[]) {
    if (!p->is_identified) { net_send_all(p->socket_fd, "ERROR MUST_HELLO\n"); strike(p, rooms, games, NULL, NULL); return; }
    if (p->current_room_id != -1) { net_send_all(p->socket_fd, "ERROR ALREADY_IN_ROOM\n"); strike(p, rooms, games, NULL, NULL); return; }

    Room *r = allocate_room(rooms);
    if (!r) { net_send_all(p->socket_fd, "ERROR NO_ROOMS\n"); return; }

    room_mark_up(r, 0, p->socket_fd, p->player_name);
    r->state = ROOM_WAITING;
    room_set_phase(r, PHASE_LOBBY);

    p->current_room_id = r->id;
    p->player_slot = 0;

    Game *g = game_for_room(r, games);
    if (g) game_room_init(g, r->id);
    r->game_active = 1;

    char out[128];
    snprintf(out, sizeof(out), "CREATED %d\nJOINED %d 1\nWAIT\n", r->id, r->id);
    net_send_all(p->socket_fd, out);

    log_info("room=%d created by fd=%d (%s)", r->id, p->socket_fd, p->player_name);
}

static void cmd_join(Player *p, Room rooms[], Game games[], Player players[], int room_id) {
    if (!p->is_identified) { net_send_all(p->socket_fd, "ERROR MUST_HELLO\n"); strike(p, rooms, games, players, NULL); return; }
    if (p->current_room_id != -1) { net_send_all(p->socket_fd, "ERROR ALREADY_IN_ROOM\n"); strike(p, rooms, games, players, NULL); return; }

    Room *r = find_room_by_id(rooms, room_id);
    if (!r) { net_send_all(p->socket_fd, "ERROR ROOM_NOT_FOUND\n"); return; }

    if (r->player_names[1][0] != '\0') { net_send_all(p->socket_fd, "ERROR ROOM_FULL\n"); return; }
    if (r->player_names[0][0] == '\0') { net_send_all(p->socket_fd, "ERROR ROOM_BROKEN\n"); return; }

    room_mark_up(r, 1, p->socket_fd, p->player_name);
    r->state = ROOM_FULL;
    room_set_phase(r, PHASE_SETUP);

    p->current_room_id = r->id;
    p->player_slot = 1;
    p->connected = 1;

    Game *g = game_for_room(r, games);
    if (g && !g->in_use) game_room_init(g, r->id);
    r->game_active = 1;

    // message for joiner
    char out[128];
    snprintf(out, sizeof(out), "JOINED %d 2\nSETUP\n", r->id);
    net_send_all(p->socket_fd, out);

    // message for host (send directly to stored fd, do NOT rely on player lookup)
    if (r->slot_connected[0] && r->player_fds[0] >= 0) {
        char out2[128];
        snprintf(out2, sizeof(out2), "JOINED %d 1\nSETUP\n", r->id);
        net_send_all(r->player_fds[0], out2);
    }

    log_info("player fd=%d (%s) joined room=%d as P2", p->socket_fd, p->player_name, r->id);
}


static void cmd_rejoin(Player *p, Room rooms[], Game games[], Player players[], int room_id) {
    if (!p->is_identified) { net_send_all(p->socket_fd, "ERROR MUST_HELLO\n"); strike(p, rooms, games, players, NULL); return; }
    if (p->current_room_id != -1) { net_send_all(p->socket_fd, "ERROR ALREADY_IN_ROOM\n"); strike(p, rooms, games, players, NULL); return; }

    Room *r = find_room_by_id(rooms, room_id);
    if (!r) { net_send_all(p->socket_fd, "ERROR ROOM_NOT_FOUND\n"); return; }

    int slot = room_slot_by_nick(r, p->player_name);
    if (slot < 0) { net_send_all(p->socket_fd, "ERROR REJOIN_DENIED\n"); return; }
    if (r->slot_connected[slot]) { net_send_all(p->socket_fd, "ERROR SLOT_ALREADY_UP\n"); return; }

    Player *old = find_disconnected_player_by_nick(players, p->player_name, r->id, slot);
    if (old) {
        old->is_identified = 0;
        old->player_name[0] = '\0';
        old->current_room_id = -1;
        old->player_slot = -1;
        old->connected = 0;
        old->disconnected_at = 0;
        old->invalid_count = 0;
    }

    room_mark_up(r, slot, p->socket_fd, p->player_name);

    p->current_room_id = r->id;
    p->player_slot = slot;
    p->connected = 1;

    char out[128];
    snprintf(out, sizeof(out), "OK REJOINED %d %d\n", r->id, slot + 1);
    net_send_all(p->socket_fd, out);

    Game *g = game_for_room(r, games);

    if (r->phase == PHASE_SETUP) {
        net_send_all(p->socket_fd, "SETUP\n");
    } else if (r->phase == PHASE_PLAY) {
        net_send_all(p->socket_fd, "PLAY\n");
        game_send_turn(g, r, players);
        if(g)game_send_state(g, r, p);
        
    } else {
        char ph[64];
        snprintf(ph, sizeof(ph), "PHASE %s\n", room_phase_str(r->phase));
        net_send_all(p->socket_fd, ph);
    }

    notify_opponent(r, players, slot, "OPPONENT_UP\n");
    log_info("player '%s' rejoined room=%d slot=%d", p->player_name, r->id, slot);
}

static void destroy_room(Room *r, Game games[], Player players[]) {
    if (!r) return;
    Game *g = game_for_room(r, games);
    if (g) game_reset(g);

    for (int slot = 0; slot < 2; slot++) {
        int fd = r->player_fds[slot];
        if (fd >= 0) {
            Player *p = find_player_by_fd(players, fd);
            if (p) { p->current_room_id = -1; p->player_slot = -1; }
        }
    }

    room_reset(r);
}

static void cmd_leave(Player *p, Room rooms[], Game games[], Player players[]) {
    if (!p->is_identified) { net_send_all(p->socket_fd, "ERROR MUST_HELLO\n"); strike(p, rooms, games, players, NULL); return; }
    if (p->current_room_id == -1) { net_send_all(p->socket_fd, "ERROR NOT_IN_ROOM\n"); strike(p, rooms, games, players, NULL); return; }

    int rid = p->current_room_id;
    Room *r = find_room_by_id(rooms, rid);

    char out[128];
    snprintf(out, sizeof(out), "LEFT %d\n", rid);
    net_send_all(p->socket_fd, out);

    if (r) {
    int opp_slot = (p->player_slot == 0) ? 1 : 0;

    int opp_fd = r->player_fds[opp_slot];
    if (opp_fd >= 0) {
        net_send_all(opp_fd, "OPPONENT_LEFT\n");
    }

    log_info("room=%d destroyed by LEAVE", rid);
    destroy_room(r, games, players);
}


    p->current_room_id = -1;
    p->player_slot = -1;
    p->connected = 1;
}


static void cmd_place(Player *p, Room rooms[], Game games[], Player players[], int x, int y, int len, char dir) {
    if (!p->is_identified) { net_send_all(p->socket_fd, "ERROR MUST_HELLO\n"); strike(p, rooms, games, players, NULL); return; }
    if (p->current_room_id == -1) { net_send_all(p->socket_fd, "ERROR NOT_IN_ROOM\n"); strike(p, rooms, games, players, NULL); return; }

    Room *r = find_room_by_id(rooms, p->current_room_id);
    if (!r) { net_send_all(p->socket_fd, "ERROR ROOM_NOT_FOUND\n"); return; }
    if (r->phase != PHASE_SETUP) { net_send_all(p->socket_fd, "ERROR BAD_STATE\n"); strike(p, rooms, games, players, NULL); return; }

    Game *g = game_for_room(r, games);
    if (!g || !g->in_use) { net_send_all(p->socket_fd, "ERROR NO_GAME\n"); return; }

    // Only valid inside PLACING_START..PLACING_STOP batch.
    if (!p->placing_mode) {
        net_send_all(p->socket_fd, "ERROR PLACE NOT_PLACING\n");
        strike(p, rooms, games, players, NULL);
        return;
    }

    if (p->pending_count >= PENDING_MAX) {
        net_send_all(p->socket_fd, "ERROR SHIPS TOO_MANY\n");
        strike(p, rooms, games, players, NULL);
        return;
    }

    PendingShip *ps = &p->pending[p->pending_count++];
    ps->x = x;
    ps->y = y;
    ps->len = len;
    ps->dir = dir;
}



static void cmd_placing(Player *p, Room rooms[], Game games[], Player players[]) {
    if (!p->is_identified) { net_send_all(p->socket_fd, "ERROR MUST_HELLO\n"); strike(p, rooms, games, players, NULL); return; }
    if (p->current_room_id == -1) { net_send_all(p->socket_fd, "ERROR NOT_IN_ROOM\n"); strike(p, rooms, games, players, NULL); return; }

    Room *r = find_room_by_id(rooms, p->current_room_id);
    if (!r) { net_send_all(p->socket_fd, "ERROR ROOM_NOT_FOUND\n"); return; }
    if (r->phase != PHASE_SETUP) { net_send_all(p->socket_fd, "ERROR BAD_STATE\n"); strike(p, rooms, games, players, NULL); return; }

    Game *g = game_for_room(r, games);
    if (!g || !g->in_use) { net_send_all(p->socket_fd, "ERROR NO_GAME\n"); return; }

    // Starting a batch ALWAYS resets only THIS player's setup so player2 never appends to player1.
    game_clear_player_setup(g, p->player_slot);
    p->placing_mode = 1;
    p->pending_count = 0;
    memset(p->pending, 0, sizeof(p->pending));

    net_send_all(p->socket_fd, "PLACING_START\n");
}



static void cmd_placing_stop(Player *p, Room rooms[], Game games[], Player players[]) {
    if (!p->is_identified) { net_send_all(p->socket_fd, "ERROR MUST_HELLO\n"); strike(p, rooms, games, players, NULL); return; }
    if (p->current_room_id == -1) { net_send_all(p->socket_fd, "ERROR NOT_IN_ROOM\n"); strike(p, rooms, games, players, NULL); return; }

    Room *r = find_room_by_id(rooms, p->current_room_id);
    if (!r) { net_send_all(p->socket_fd, "ERROR ROOM_NOT_FOUND\n"); return; }
    if (r->phase != PHASE_SETUP) { net_send_all(p->socket_fd, "ERROR BAD_STATE\n"); strike(p, rooms, games, players, NULL); return; }

    Game *g = game_for_room(r, games);
    if (!g || !g->in_use) { net_send_all(p->socket_fd, "ERROR NO_GAME\n"); return; }

    if (!p->placing_mode) { net_send_all(p->socket_fd, "ERROR SHIPS NOT_PLACING\n"); strike(p, rooms, games, players, NULL); return; }
    if (p->pending_count != PENDING_MAX) { net_send_all(p->socket_fd, "ERROR SHIPS INCOMPLETE\n"); strike(p, rooms, games, players, NULL); return; }

    // apply buffered ships to THIS player's board
    for (int i = 0; i < p->pending_count; i++) {
        PendingShip *ps = &p->pending[i];
        char err[64];

        if (!game_place_ship(g, p->player_slot, ps->x, ps->y, ps->len, ps->dir, err, sizeof(err))) {
            net_send_all(p->socket_fd, "ERROR SHIPS ");
            net_send_all(p->socket_fd, err);
            net_send_all(p->socket_fd, "\n");

            // keep it clean on failure (only this player)
            game_clear_player_setup(g, p->player_slot);
            pending_reset(p);
            strike(p, rooms, games, players, NULL);
            return;
        }
    }

    pending_reset(p);

    // Mark this player as ready internally (no READY command exposed to client).
    {
        char err2[64];
        if (!game_set_ready(g, p->player_slot, err2, sizeof(err2))) {
            net_send_all(p->socket_fd, "ERROR SHIPS ");
            net_send_all(p->socket_fd, err2);
            net_send_all(p->socket_fd, "\n");
            strike(p, rooms, games, players, NULL);
            return;
        }
    }

    net_send_all(p->socket_fd, "SHIPS_OK\n");

    // If both players finished placing -> start game.
    if (game_all_ready(g)) {
        room_set_phase(r, PHASE_PLAY);

        for (int slot = 0; slot < 2; slot++) {
            if (!r->slot_connected[slot]) continue;
            int fd = r->player_fds[slot];
            if (fd >= 0) net_send_all(fd, "PLAY\n");
        }

        // Send YOUR_TURN / OPP_TURN (handled inside game_send_turn)
        game_send_turn(g, r, players);
    }
}




static void cmd_ready(Player *p, Room rooms[], Game games[], Player players[]) {
    (void)rooms; (void)games; (void)players;
    if (!p || p->socket_fd < 0) return;
    // READY is intentionally disabled: ships placement completion is the only "ready" path.
    net_send_all(p->socket_fd, "ERROR READY_DISABLED\n");
}



static void cmd_shoot(Player *p, Room rooms[], Game games[], Player players[], int x, int y)
{
    if (!p->is_identified) { net_send_all(p->socket_fd, "ERROR MUST_HELLO\n"); strike(p, rooms, games, players, NULL); return; }
    if (p->current_room_id == -1) { net_send_all(p->socket_fd, "ERROR NOT_IN_ROOM\n"); strike(p, rooms, games, players, NULL); return; }

    Room *r = find_room_by_id(rooms, p->current_room_id);
    if (!r) { net_send_all(p->socket_fd, "ERROR ROOM_NOT_FOUND\n"); return; }
    if (r->phase != PHASE_PLAY) { net_send_all(p->socket_fd, "ERROR BAD_STATE\n"); strike(p, rooms, games, players, NULL); return; }

    Game *g = game_for_room(r, games);
    if (!g || !g->in_use) { net_send_all(p->socket_fd, "ERROR NO_GAME\n"); return; }

    char err[64];
    int res = game_shoot(g, p->player_slot, x, y, err, sizeof(err));
    if (res < 0) {
        char out[96];
        snprintf(out, sizeof(out), "ERROR SHOOT %s\n", err);
        net_send_all(p->socket_fd, out);
        strike(p, rooms, games, players, NULL);
        return;
    }

    if (res == 0) {
        net_send_all(p->socket_fd, "WATER\n");
        notify_opponent(r, players, p->player_slot, "OPP_WATER\n");

    } else if (res == 1) {
        net_send_all(p->socket_fd, "HIT\n");
        notify_opponent(r, players, p->player_slot, "OPP_HIT\n");

    } else if (res == 2) {
        // --- SUNK x y len dir ---
        int victim_slot = 1 - p->player_slot;
        unsigned char sid = g->ship_id[victim_slot][y][x];

        Player *opponent = &players[r->player_fds[victim_slot]];

        game_send_sunk_def(g, victim_slot, sid, p, opponent);

    } else if (res == 3) {
        net_send_all(p->socket_fd, "WIN\n");
        notify_opponent(r, players, p->player_slot, "LOSE\n");
        room_set_phase(r, PHASE_FINISHED);
    }

    game_send_turn(g, r, players);
}

static void cmd_state(Player *p, Room rooms[], Game games[]) {
    if (!p->is_identified) { net_send_all(p->socket_fd, "ERROR MUST_HELLO\n"); strike(p, rooms, games, NULL, NULL); return; }
    if (p->current_room_id == -1) { net_send_all(p->socket_fd, "ERROR NOT_IN_ROOM\n"); strike(p, rooms, games, NULL, NULL); return; }

    Room *r = find_room_by_id(rooms, p->current_room_id);
    if (!r) { net_send_all(p->socket_fd, "ERROR ROOM_NOT_FOUND\n"); return; }
    Game *g = game_for_room(r, games);
    if (g) game_send_state(g, r, p);
}

// --- public API ---

void protocol_handle_line(Player *p, Room rooms[], Game games[], Player players[], const char *line) {
    log_info("rx fd=%d line='%s'", p->socket_fd, line);

    char cmd[32] = {0};
    if (sscanf(line, "%31s", cmd) != 1) {
        net_send_all(p->socket_fd, "ERROR BAD_COMMAND\n");
        strike(p, rooms, games, players, NULL);
        return;
    }

    if (strcmp(cmd, "HELLO") == 0) {
        const char *sp = strchr(line, ' ');
        if (!sp) { net_send_all(p->socket_fd, "ERROR BAD_ARGS\n"); strike(p, rooms, games, players, NULL); return; }
        cmd_hello(p, sp + 1);
        return;
    }

    if (strcmp(cmd, "LIST") == 0) { cmd_list(p, rooms, games, players); return; }
    if (strcmp(cmd, "CREATE") == 0) { cmd_create(p, rooms, games); return; }

    if (strcmp(cmd, "JOIN") == 0) {
        int rid = -1;
        if (sscanf(line, "JOIN %d", &rid) != 1) { net_send_all(p->socket_fd, "ERROR BAD_ARGS\n"); strike(p, rooms, games, players, NULL); return; }
        cmd_join(p, rooms, games, players, rid);
        return;
    }

    if (strcmp(cmd, "REJOIN") == 0) {
        int rid = -1;
        if (sscanf(line, "REJOIN %d", &rid) != 1) { net_send_all(p->socket_fd, "ERROR BAD_ARGS\n"); strike(p, rooms, games, players, NULL); return; }
        cmd_rejoin(p, rooms, games, players, rid);
        return;
    }

    if (strcmp(cmd, "LEAVE") == 0) { cmd_leave(p, rooms, games, players); return; }
    if (strcmp(cmd, "PLACING_START") == 0 || strcmp(cmd, "PLACING") == 0) { cmd_placing(p, rooms, games, players); return; }
    if (strcmp(cmd, "PLACING_STOP") == 0 || strcmp(cmd, "PLACING_END") == 0) { cmd_placing_stop(p, rooms, games, players); return; }

    if (strcmp(cmd, "PLACE") == 0) {
        int x,y,len; char dir;
        if (sscanf(line, "PLACE %d %d %d %c", &x, &y, &len, &dir) != 4) { net_send_all(p->socket_fd, "ERROR BAD_ARGS\n"); strike(p, rooms, games, players, NULL); return; }
        cmd_place(p, rooms, games, players, x, y, len, dir);
        return;
    }

    if (strcmp(cmd, "READY") == 0) { cmd_ready(p, rooms, games, players); return; }

    if (strcmp(cmd, "SHOOT") == 0) {
        int x,y;
        if (sscanf(line, "SHOOT %d %d", &x, &y) != 2) { net_send_all(p->socket_fd, "ERROR BAD_ARGS\n"); strike(p, rooms, games, players, NULL); return; }
        cmd_shoot(p, rooms, games, players, x, y);
        return;
    }

    if (strcmp(cmd, "STATE") == 0) { cmd_state(p, rooms, games); return; }

    net_send_all(p->socket_fd, "ERROR BAD_COMMAND\n");
    strike(p, rooms, games, players, NULL);
}

void protocol_process_incoming(Player *p, Room rooms[], Game games[], Player players[]) {
    ssize_t r = recv(p->socket_fd, p->rx_buffer + p->rx_len, BUF_SIZE - p->rx_len, 0);

    if (r == 0) {
        log_info("fd=%d disconnected (soft)", p->socket_fd);

        if (p->current_room_id != -1) {
            Room *rm = find_room_by_id(rooms, p->current_room_id);
            if (rm && p->player_slot >= 0) {
                if (rm->phase == PHASE_SETUP || rm->phase == PHASE_PLAY) {
                    room_mark_down(rm, p->player_slot);
                    notify_opponent(rm, players, p->player_slot, "OPPONENT_DOWN\n");
                    player_soft_disconnect(p);
                    return;
                }

                // immediate close outside SETUP/PLAY
                log_info("room=%d phase=%s: immediate close on disconnect", rm->id, room_phase_str(rm->phase));
                room_mark_down(rm, p->player_slot);
                player_soft_disconnect(p);           // <- make this slot disconnected
                close_room_now(rm, games, players, "DISCONNECT");
                return;
            }
        }

        player_reset(p);
        return;
    }

    if (r < 0) {
        if (errno == EINTR) return;
        log_error("fd=%d recv error -> soft disconnect", p->socket_fd);

        if (p->current_room_id != -1) {
            Room *rm = find_room_by_id(rooms, p->current_room_id);
            if (rm && p->player_slot >= 0) {
                if (rm->phase == PHASE_SETUP || rm->phase == PHASE_PLAY) {
                    room_mark_down(rm, p->player_slot);
                    notify_opponent(rm, players, p->player_slot, "OPPONENT_DOWN\n");
                    player_soft_disconnect(p);
                    return;
                }

                log_info("room=%d phase=%s: immediate close on recv error", rm->id, room_phase_str(rm->phase));
                room_mark_down(rm, p->player_slot);
                player_soft_disconnect(p);
                close_room_now(rm, games, players, "DISCONNECT");
                return;
            }
        }

        player_reset(p);
        return;
    }

    p->rx_len += (size_t)r;

    size_t start = 0;
    for (size_t i = 0; i < p->rx_len; i++) {
        if (p->rx_buffer[i] == '\n') {
            size_t line_len = i - start;
            char line[1024];
            if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;

            memcpy(line, p->rx_buffer + start, line_len);
            line[line_len] = '\0';

            protocol_handle_line(p, rooms, games, players, line);
            start = i + 1;

            if (p->socket_fd < 0) return;
        }
    }

    if (start > 0) {
        size_t rem = p->rx_len - start;
        memmove(p->rx_buffer, p->rx_buffer + start, rem);
        p->rx_len = rem;
    }

    if (p->rx_len == BUF_SIZE) {
        net_send_all(p->socket_fd, "ERROR LINE_TOO_LONG\n");
        log_error("fd=%d line too long -> hard disconnect", p->socket_fd);

        if (p->current_room_id != -1) {
            Room *rm = find_room_by_id(rooms, p->current_room_id);
            if (rm) destroy_room(rm, games, players);
        }
        player_reset(p);
    }
}
