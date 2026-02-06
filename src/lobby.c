#include "lobby.h"
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void room_reset(Room *r) {
  // Reset celé roomky do výchozího stavu (jako „prázdný slot“)
  r->state = ROOM_EMPTY;
  r->phase = PHASE_LOBBY;
  r->id = 0;

  r->player_fds[0] = -1;
  r->player_fds[1] = -1;
  memset(r->player_names, 0, sizeof(r->player_names));

  r->slot_connected[0] = 0;
  r->slot_connected[1] = 0;
  r->slot_down_since[0] = 0;
  r->slot_down_since[1] = 0;

  r->game_active = 0;
}

static const char *room_state_str(RoomState st) {
  return (st == ROOM_WAITING) ? "WAITING"
         : (st == ROOM_FULL)  ? "FULL"
                              : "EMPTY";
}

const char *room_phase_str(RoomPhase ph) {
  return (ph == PHASE_LOBBY)      ? "LOBBY"
         : (ph == PHASE_SETUP)    ? "SETUP"
         : (ph == PHASE_PLAY)     ? "PLAY"
         : (ph == PHASE_FINISHED) ? "FINISHED"
                                  : "UNKNOWN";
}

static int room_player_count(const Room *r) {
  // Počítáme hráče podle toho, jestli mají vyplněný nick (nezávisle na fd)
  int c = 0;
  if (r->player_names[0][0]) c++;
  if (r->player_names[1][0]) c++;
  return c;
}

Room *find_room_by_id(Room rooms[], int room_id) {
  for (int i = 0; i < MAX_ROOMS; i++)
    if (rooms[i].state != ROOM_EMPTY && rooms[i].id == room_id)
      return &rooms[i];
  return NULL;
}

Room *allocate_room(Room rooms[]) {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (rooms[i].state == ROOM_EMPTY) {
      rooms[i].id = i + 1;
      rooms[i].state = ROOM_WAITING;
      rooms[i].phase = PHASE_LOBBY;

      rooms[i].player_fds[0] = -1;
      rooms[i].player_fds[1] = -1;
      memset(rooms[i].player_names, 0, sizeof(rooms[i].player_names));

      rooms[i].slot_connected[0] = 0;
      rooms[i].slot_connected[1] = 0;
      rooms[i].slot_down_since[0] = 0;
      rooms[i].slot_down_since[1] = 0;

      rooms[i].game_active = 0;
      return &rooms[i];
    }
  }
  return NULL;
}

void room_mark_down(Room *r, int slot) {
  if (!r || slot < 0 || slot > 1) return;

  // Slot je „DOWN“: zneplatníme fd a uložíme čas výpadku (kvůli timeoutům / rejoin)
  r->slot_connected[slot] = 0;
  if (r->slot_down_since[slot] == 0)
    r->slot_down_since[slot] = time(NULL);

  r->player_fds[slot] = -1;
}

void room_mark_up(Room *r, int slot, int fd, const char *nick) {
  if (!r || slot < 0 || slot > 1) return;

  r->player_fds[slot] = fd;
  r->slot_connected[slot] = 1;
  r->slot_down_since[slot] = 0;

  // Nick držíme i při reconnectech, aby šlo hráče znovu spárovat do správného slotu
  if (nick && nick[0]) {
    snprintf(r->player_names[slot], sizeof(r->player_names[slot]), "%s", nick);
  }
}

int room_slot_by_nick(Room *r, const char *nick) {
  if (!r || !nick) return -1;

  if (r->player_names[0][0] && strcmp(r->player_names[0], nick) == 0) return 0;
  if (r->player_names[1][0] && strcmp(r->player_names[1], nick) == 0) return 1;

  return -1;
}

void lobby_send_room_list(int to_fd, Room rooms[]) {
  int count = 0;
  for (int i = 0; i < MAX_ROOMS; i++)
    if (rooms[i].state != ROOM_EMPTY)
      count++;

  char line[200];
  snprintf(line, sizeof(line), "ROOMS %d\n", count);
  net_send_all(to_fd, line);

  for (int i = 0; i < MAX_ROOMS; i++) {
    if (rooms[i].state == ROOM_EMPTY) continue;

    snprintf(line, sizeof(line), "ROOM %d %d %s %s P1=%s P2=%s\n",
             rooms[i].id,
             room_player_count(&rooms[i]),
             room_state_str(rooms[i].state),
             room_phase_str(rooms[i].phase),
             rooms[i].slot_connected[0] ? "UP" : "DOWN",
             rooms[i].slot_connected[1] ? "UP" : "DOWN");
    net_send_all(to_fd, line);
  }
}
