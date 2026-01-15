#define _POSIX_C_SOURCE 200112L
#include "common.h"
#include "net.h"
#include "lobby.h"
#include "game.h"
#include "protocol.h"
#include "log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RECONNECT_GRACE_SEC 45

static void die(const char *msg) { perror(msg); exit(1); }

static void tick(Room rooms[], Game games[], Player players[]) {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_ROOMS; i++) {
        Room *r = &rooms[i];
        if (r->state == ROOM_EMPTY) continue;

        for (int slot = 0; slot < 2; slot++) {
            if (r->slot_connected[slot]) continue;
            if (r->player_names[slot][0] == '\0') continue;
            if (r->slot_down_since[slot] == 0) continue;

            double dt = difftime(now, r->slot_down_since[slot]);
            if (dt < RECONNECT_GRACE_SEC) continue;

            int opp = (slot == 0) ? 1 : 0;
            if (r->slot_connected[opp]) {
                Player *op = find_player_by_fd(players, r->player_fds[opp]);
                if (op) {
                    net_send_all(op->socket_fd, "OPPONENT_TIMEOUT\n");
                    net_send_all(op->socket_fd, "ROOM_CLOSED TIMEOUT\n");
                    net_send_all(op->socket_fd, "RETURNED_TO_LOBBY\n");
                }
            }

            log_info("room=%d slot=%d timeout -> destroy", r->id, slot);
            // destroy room + game
            Game *g = &games[r->id - 1];
            game_reset(g);

            // Detach ALL players bound to this room (connected and disconnected).
            // Important: disconnected players still occupy a Player slot (reserved for REJOIN).
            for (int pi = 0; pi < MAX_PLAYERS; pi++) {
                Player *pp = &players[pi];
                if (pp->is_identified && pp->current_room_id == r->id) {
                    // if still connected, tell them explicitly
                    if (pp->socket_fd >= 0) {
                        net_send_all(pp->socket_fd, "ROOM_CLOSED TIMEOUT\n");
                        net_send_all(pp->socket_fd, "RETURNED_TO_LOBBY\n");
                    }
                    // hard reset the slot so it can be reused
                    if (pp->socket_fd >= 0) {
                        // still connected -> do NOT close fd
                        pp->current_room_id = -1;
                        pp->player_slot = -1;
                    }else {
                        // ghost slot reserved for rejoin (socket_fd == -1) or marked disconnected
                        // room is gone -> free it
                        player_reset(pp);}

                }
                
            }

            room_reset(r);
            break;
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\nExample: %s 0.0.0.0 5555\n", argv[0], argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    if (port <= 0 || port > 65535) { fprintf(stderr, "Bad port\n"); return 1; }

    int listen_fd = net_make_listen_socket(ip, port);
    log_info("server listening on %s:%d", ip, port);

    Player players[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) { players[i].socket_fd = -1; player_reset(&players[i]); }

    Room rooms[MAX_ROOMS];
    for (int i = 0; i < MAX_ROOMS; i++) room_reset(&rooms[i]);

    Game games[MAX_ROOMS];
    for (int i = 0; i < MAX_ROOMS; i++) game_reset(&games[i]);

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);

        FD_SET(listen_fd, &readfds);
        int maxfd = listen_fd;

        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].socket_fd >= 0) {
                FD_SET(players[i].socket_fd, &readfds);
                if (players[i].socket_fd > maxfd) maxfd = players[i].socket_fd;
            }
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int rc = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (rc < 0) { if (errno == EINTR) continue; die("select"); }

        // periodic maintenance
        tick(rooms, games, players);
        // heartbeat: server pinguje klienty, po 3 missed PONG => opponent down
        protocol_heartbeat_tick(rooms, games, players);


        // new player
        if (rc > 0 && FD_ISSET(listen_fd, &readfds)) {
            int new_fd = accept(listen_fd, NULL, NULL);
            if (new_fd >= 0) {
                int placed = 0;
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    // only truly free slots (not reserved for reconnect)
                    if (players[i].socket_fd < 0 && players[i].is_identified == 0) {
                        players[i].socket_fd = new_fd;
                        players[i].rx_len = 0;
                        players[i].is_identified = 0;
                        players[i].player_name[0] = '\0';
                        players[i].current_room_id = -1;
                        players[i].player_slot = -1;
                        players[i].invalid_count = 0;
                        players[i].connected = 1;
                        players[i].disconnected_at = 0;
                        placed = 1;
                        break;
                    }
                }

                if (!placed) {
                    net_send_all(new_fd, "ERROR SERVER_FULL\n");
                    close(new_fd);
                    log_warn("rejecting fd=%d (server full)", new_fd);
                } else {
                    log_info("player connected fd=%d", new_fd);
                }
            }
        }

        // data from players
        if (rc > 0) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (players[i].socket_fd >= 0 && FD_ISSET(players[i].socket_fd, &readfds)) {
                    protocol_process_incoming(&players[i], rooms, games, players);
                }
            }
        }
    }

    close(listen_fd);
    return 0;
}
