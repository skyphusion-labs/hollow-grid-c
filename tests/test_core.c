#include "hg_event.h"
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

static void test_store(void) {
  char root[128];
  snprintf(root, sizeof(root), "/tmp/hollow-grid-c-unit-%ld", (long)getpid());

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
  assert(hg_store_save(&store, &saved) == 0);

  hg_character loaded;
  assert(hg_store_load(&store, "ferrite_17", &loaded) == 1);
  assert(strcmp(loaded.race, "elf") == 0);
  assert(strcmp(loaded.room, "coil-yard") == 0);
  assert(strcmp(loaded.weapon, "shiv") == 0);
  assert(loaded.inventory_count == 1);

  char record[640];
  snprintf(record, sizeof(record), "%s/ferrite_17.json", store.root);
  assert(remove(record) == 0);
  assert(rmdir(store.root) == 0);
  assert(rmdir(root) == 0);
}

int main(void) {
  test_world();
  test_items();
  test_event();
  test_store();
  puts("core tests passed");
  return 0;
}
