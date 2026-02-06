#pragma once

#include "common.h"
#include <time.h>

typedef enum { ROOM_EMPTY = 0, ROOM_WAITING = 1, ROOM_FULL = 2 } RoomState;

typedef enum {
  PHASE_LOBBY = 0,
  PHASE_SETUP = 1,
  PHASE_PLAY = 2,
  PHASE_FINISHED = 3
} RoomPhase;

typedef struct Room {
  int id;
  RoomState state;
  RoomPhase phase;

  int player_fds[2];

  // reconnect metadata
  char player_names[2][32];
  int slot_connected[2];
  time_t slot_down_since[2];

  // game link
  int game_active;
} Room;

void room_reset(Room *r);
Room *allocate_room(Room rooms[]);
Room *find_room_by_id(Room rooms[], int room_id);

const char *room_phase_str(RoomPhase ph);

void room_mark_down(Room *r, int slot);
void room_mark_up(Room *r, int slot, int fd, const char *nick);
int room_slot_by_nick(Room *r, const char *nick);

void lobby_send_room_list(int to_fd, Room rooms[]);
