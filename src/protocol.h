#pragma once

#include "lobby.h"
#include "net.h"
#include "game.h"

void protocol_handle_line(Player *p, Room rooms[], Game games[], Player players[], const char *line);
void protocol_process_incoming(Player *p, Room rooms[], Game games[], Player players[]);
void protocol_heartbeat_tick(Room rooms[], Game games[], Player players[]);
static void heartbeat_soft_disconnect(Player *p, Room rooms[], Game games[], Player players[]);
