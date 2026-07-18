#ifndef HG_WORLD_H
#define HG_WORLD_H

#include <stddef.h>
#include <time.h>

#define HG_MAX_ACTIONS 2
#define HG_MAX_EXITS 5
#define HG_MAX_LIVE_MOBS 16

typedef struct {
  const char *verb;
  const char *label;
  const char *kind;
  const char *valence;
} hg_action;

typedef struct {
  const char *direction;
  const char *destination;
} hg_exit;

typedef struct {
  const char *id;
  const char *name;
  const char *description;
  int outdoors;
  hg_exit exits[HG_MAX_EXITS];
  size_t exit_count;
  hg_action actions[HG_MAX_ACTIONS];
  size_t action_count;
} hg_room;

typedef struct {
  char id[16];
  char name[48];
  char description[256];
  char room[32];
  int hp;
  int max_hp;
  int damage;
  int xp;
  int alive;
  int respawn_ms;
  time_t died_at;
} hg_live_mob;

typedef struct {
  hg_live_mob mobs[HG_MAX_LIVE_MOBS];
  size_t mob_count;
  time_t started;
} hg_world_state;

void hg_world_init(hg_world_state *state);
const hg_room *hg_world_start(void);
const hg_room *hg_world_room(const char *id);
const hg_room *hg_world_move(const hg_room *from, const char *direction);
hg_live_mob *hg_world_mob_in_room(hg_world_state *state, const char *room_id,
                                  const char *mob_id);
size_t hg_world_mobs_in_room(hg_world_state *state, const char *room_id,
                             hg_live_mob **out, size_t out_cap);
void hg_world_tick_respawns(hg_world_state *state);

#endif
