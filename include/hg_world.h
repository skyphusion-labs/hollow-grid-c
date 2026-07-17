#ifndef HG_WORLD_H
#define HG_WORLD_H

#include <stddef.h>

#define HG_MAX_ACTIONS 2
#define HG_MAX_EXITS 5
#define HG_MAX_MOBS 2

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
  int hp;
  int max_hp;
} hg_mob;

typedef struct {
  const char *id;
  const char *name;
  const char *description;
  int outdoors;
  hg_exit exits[HG_MAX_EXITS];
  size_t exit_count;
  hg_mob mobs[HG_MAX_MOBS];
  size_t mob_count;
  hg_action actions[HG_MAX_ACTIONS];
  size_t action_count;
} hg_room;

const hg_room *hg_world_start(void);
const hg_room *hg_world_room(const char *id);
const hg_room *hg_world_move(const hg_room *from, const char *direction);

#endif
