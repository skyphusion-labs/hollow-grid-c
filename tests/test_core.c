#include "hg_event.h"
#include "hg_grid.h"
#include "hg_items.h"
#include "hg_store.h"
#include "hg_world.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_world(void) {
  hg_world_state state;
  hg_world_init(&state);

  const hg_room *nexus = hg_world_start();
  assert(nexus != NULL);
  assert(strcmp(nexus->id, "nexus") == 0);
  assert(strcmp(hg_world_move(nexus, "east")->id, "workshop") == 0);

  const hg_room *roof = hg_world_room("roof");
  const hg_room *coil_yard = hg_world_move(roof, "west");
  assert(coil_yard != NULL);
  assert(strcmp(coil_yard->id, "coil-yard") == 0);

  const hg_room *dunes = hg_world_room("dunes");
  assert(strcmp(hg_world_move(dunes, "east")->id, "scorch_road") == 0);
  assert(strcmp(hg_world_move(hg_world_room("scorch_road"), "east")->id,
                "waystation") == 0);

  hg_live_mob *rat = hg_world_mob_in_room(&state, "tunnels", "rat");
  assert(rat != NULL);
  assert(strstr(rat->description, "rodent") != NULL);
  assert(hg_world_move(nexus, "up") == NULL);
}

static void test_items(void) {
  const hg_item *shiv = hg_item_by_id("shiv");
  assert(shiv != NULL);
  assert(strcmp(shiv->slot, "weapon") == 0);
  assert(shiv->damage == 3);
}

static void test_event(void) {
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "id", "nexus");
  char *line = hg_event_line("room.info", payload);
  assert(line != NULL);
  assert(strncmp(line, "@event room.info ", 17) == 0);
  assert(strstr(line, "{\"id\":\"nexus\"}") != NULL);
  assert(strcmp(line + strlen(line) - 2, "\r\n") == 0);
  free(line);
}

static void rm_rf(const char *path) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
  /* Assign then discard: a bare (void)system(...) still trips GCC's
     warn_unused_result. Cleanup failure is non-fatal for a temp dir. */
  int rc = system(cmd);
  (void)rc;
}

static void test_store(void) {
  char root[128];
  snprintf(root, sizeof(root), "/tmp/hollow-grid-c-unit-%ld", (long)getpid());
  rm_rf(root);

  hg_store store;
  assert(hg_store_init(&store, root) == 0);

  hg_character saved;
  hg_character_new(&saved, "Ferrite_17");
  assert(saved.inventory_count == 1);
  assert(strcmp(saved.inventory[0], "shiv") == 0);
  snprintf(saved.race, sizeof(saved.race), "elf");
  snprintf(saved.room, sizeof(saved.room), "coil-yard");
  snprintf(saved.weapon, sizeof(saved.weapon), "shiv");
  saved.morality = 3;
  saved.xp = 42;
  saved.gold = 7;
  saved.ashsworn = 1;
  saved.strayed = 1;
  saved.redeemed = 1;
  snprintf(saved.title, sizeof(saved.title), "the Unbowed");
  assert(hg_character_add_item(&saved, "antidote") == 0);
  assert(hg_store_save(&store, &saved) == 0);

  hg_character loaded;
  assert(hg_store_load(&store, "ferrite_17", &loaded) == 1);
  assert(strcmp(loaded.race, "elf") == 0);
  assert(strcmp(loaded.room, "coil-yard") == 0);
  assert(strcmp(loaded.weapon, "shiv") == 0);
  assert(strcmp(loaded.title, "the Unbowed") == 0);
  assert(loaded.xp == 42);
  assert(loaded.gold == 7);
  assert(loaded.ashsworn == 1);
  assert(loaded.strayed == 1);
  assert(loaded.redeemed == 1);
  assert(loaded.inventory_count == 2);
  assert(hg_character_has_item(&loaded, "antidote") == 1);

  /* A name with no record loads as "not found" (0), not an error. */
  assert(hg_store_load(&store, "no_such_soul", &loaded) == 0);

  /* Lookups are case-insensitive on the stored key. */
  assert(hg_store_load(&store, "FERRITE_17", &loaded) == 1);
  assert(strcmp(loaded.race, "elf") == 0);

  /* Re-save overwrites in place (UPSERT), no duplicate row. */
  loaded.gold = 99;
  assert(hg_store_save(&store, &loaded) == 0);
  hg_character again;
  assert(hg_store_load(&store, "ferrite_17", &again) == 1);
  assert(again.gold == 99);

  hg_store_close(&store);

  /* A fresh open of the same directory sees the persisted record: SQLite
     durability across process restarts. */
  hg_store reopened;
  assert(hg_store_init(&reopened, root) == 0);
  assert(hg_store_load(&reopened, "ferrite_17", &loaded) == 1);
  assert(loaded.gold == 99);
  hg_store_close(&reopened);

  rm_rf(root);
}

/* A legacy per-character .json file left by the old file store is imported on
   first init, so live players keep their progress after the SQLite cutover. */
static void test_store_migration(void) {
  char root[128];
  snprintf(root, sizeof(root), "/tmp/hollow-grid-c-migrate-%ld",
           (long)getpid());
  rm_rf(root);

  char chars_dir[192];
  snprintf(chars_dir, sizeof(chars_dir), "%s/characters", root);
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", chars_dir);
  int mk = system(cmd);
  assert(mk == 0);

  char legacy[256];
  snprintf(legacy, sizeof(legacy), "%s/oldsoul.json", chars_dir);
  FILE *f = fopen(legacy, "wb");
  assert(f != NULL);
  fputs("{\"name\":\"OldSoul\",\"race\":\"revenant\",\"room\":\"tunnels\","
        "\"gold\":13,\"ashsworn\":true}",
        f);
  fclose(f);

  hg_store store;
  assert(hg_store_init(&store, root) == 0);
  hg_character loaded;
  assert(hg_store_load(&store, "oldsoul", &loaded) == 1);
  assert(strcmp(loaded.race, "revenant") == 0);
  assert(strcmp(loaded.room, "tunnels") == 0);
  assert(loaded.gold == 13);
  assert(loaded.ashsworn == 1);
  hg_store_close(&store);

  rm_rf(root);
}

static void test_store_names(void) {
  assert(hg_store_valid_name("ok") == 1);
  assert(hg_store_valid_name("Ferrite_17-b") == 1);
  assert(hg_store_valid_name(NULL) == 0);
  assert(hg_store_valid_name("x") == 0);
  assert(hg_store_valid_name("has space") == 0);
  assert(hg_store_valid_name("slash/name") == 0);

  char too_long[64];
  memset(too_long, 'a', sizeof(too_long));
  too_long[sizeof(too_long) - 1] = '\0';
  assert(hg_store_valid_name(too_long) == 0);

  /* hg_store_init rejects empty/NULL data dirs. */
  hg_store store;
  assert(hg_store_init(&store, NULL) == -1);
  assert(hg_store_init(&store, "") == -1);
  assert(hg_store_init(NULL, "/tmp") == -1);
}

static void test_character_items(void) {
  hg_character c;
  hg_character_new(&c, "Rivet");
  assert(c.inventory_count == 1);

  /* Fill inventory to the cap, then confirm the overflow is refused. */
  const char *ids[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i"};
  int added = 0;
  for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
    if (hg_character_add_item(&c, ids[i]) == 0) {
      added++;
    }
  }
  assert(c.inventory_count == HG_MAX_INVENTORY);
  assert(added < (int)(sizeof(ids) / sizeof(ids[0])));

  assert(hg_character_remove_item(&c, "shiv") == 0);
  assert(hg_character_has_item(&c, "shiv") == 0);
  assert(hg_character_remove_item(&c, "not-here") == -1);

  assert(hg_character_add_item(NULL, "x") == -1);
  assert(hg_character_has_item(NULL, "x") == 0);
  assert(hg_character_remove_item(NULL, "x") == -1);
}

static void test_grid_offline(void) {
  /* No hub URL means LocalHub: hg_grid_open returns NULL, callers stay local. */
  assert(hg_grid_open(NULL, NULL) == NULL);
  assert(hg_grid_open("", "tok") == NULL);
  hg_grid_close(NULL);

  /* A grid pointed at a closed port exercises the RPC transport-failure path
     without a live hub. Connection refused returns fast (no 2s timeout). */
  hg_grid *grid = hg_grid_open("http://127.0.0.1:1/rpc", "");
  assert(grid != NULL);
  int tide = 0;
  assert(hg_grid_tide(grid, &tide) == -1);
  long latency = -1;
  assert(hg_grid_ping(grid, &latency) == -1);
  assert(latency >= 0);
  assert(hg_grid_register(grid, "Ferrite Wastes", "ws://x/ws") == -1);
  assert(hg_grid_gridcast(grid, "Ferrite Wastes", "Rivet", "hi") == -1);
  hg_grid_world worlds[4];
  size_t world_count = 99;
  assert(hg_grid_list_worlds(grid, worlds, 4, &world_count) == -1);
  assert(world_count == 0);
  hg_grid_close(grid);
}

int main(void) {
  test_world();
  test_items();
  test_event();
  test_store();
  test_store_migration();
  test_store_names();
  test_character_items();
  test_grid_offline();
  puts("core tests passed");
  return 0;
}
