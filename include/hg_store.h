#ifndef HG_STORE_H
#define HG_STORE_H

#include <stddef.h>

#define HG_MAX_INVENTORY 8

typedef struct {
  char name[33];
  char race[16];
  char room[32];
  char faction[16];
  char position[16];
  char title[48];
  char inventory[HG_MAX_INVENTORY][16];
  size_t inventory_count;
  char weapon[16];
  int hp;
  int max_hp;
  int level;
  int xp;
  int gold;
  int morality;
  int addiction;
  int ashsworn;
  int strayed;
  int redeemed;
} hg_character;

typedef struct {
  char root[512];
} hg_store;

int hg_store_init(hg_store *store, const char *data_dir);
int hg_store_load(const hg_store *store, const char *name, hg_character *out);
int hg_store_save(const hg_store *store, const hg_character *character);
int hg_store_valid_name(const char *name);
void hg_character_new(hg_character *character, const char *name);
int hg_character_has_item(const hg_character *character, const char *item_id);
int hg_character_add_item(hg_character *character, const char *item_id);
int hg_character_remove_item(hg_character *character, const char *item_id);

#endif
