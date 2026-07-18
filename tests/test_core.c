#include "hg_event.h"
#include "hg_store.h"
#include "hg_world.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_world(void) {
  const hg_room *nexus = hg_world_start();
  assert(nexus != NULL);
  assert(strcmp(nexus->id, "nexus") == 0);
  assert(strcmp(hg_world_move(nexus, "east")->id, "workshop") == 0);

  const hg_room *roof = hg_world_room("roof");
  const hg_room *coil_yard = hg_world_move(roof, "west");
  assert(coil_yard != NULL);
  assert(strcmp(coil_yard->id, "coil-yard") == 0);
  assert(strcmp(hg_world_move(coil_yard, "east")->id, "roof") == 0);

  const hg_room *tunnels = hg_world_room("tunnels");
  assert(tunnels != NULL);
  assert(tunnels->mob_count == 1);
  assert(strcmp(tunnels->mobs[0].id, "rat") == 0);
  assert(hg_world_move(nexus, "up") == NULL);
}

static void test_event(void) {
  cJSON *payload = cJSON_CreateObject();
  cJSON_AddStringToObject(payload, "id", "nexus");
  char *line = hg_event_line("room.info", payload);
  assert(line != NULL);
  assert(strncmp(line, "@event room.info ", 17) == 0);
  assert(strstr(line, "{\"id\":\"nexus\"}") != NULL);
  assert(strlen(line) >= 2);
  assert(strcmp(line + strlen(line) - 2, "\r\n") == 0);
  free(line);
}

static void test_store(void) {
  char root[128];
  snprintf(root, sizeof(root), "/tmp/hollow-grid-c-unit-%ld", (long)getpid());

  hg_store store;
  assert(hg_store_init(&store, root) == 0);
  assert(hg_store_valid_name("Ferrite_17"));
  assert(!hg_store_valid_name("a"));
  assert(!hg_store_valid_name("../escape"));

  hg_character saved;
  hg_character_new(&saved, "Ferrite_17");
  snprintf(saved.race, sizeof(saved.race), "elf");
  snprintf(saved.room, sizeof(saved.room), "coil-yard");
  saved.morality = 3;
  assert(hg_store_save(&store, &saved) == 0);

  hg_character loaded;
  assert(hg_store_load(&store, "ferrite_17", &loaded) == 1);
  assert(strcmp(loaded.name, "Ferrite_17") == 0);
  assert(strcmp(loaded.race, "elf") == 0);
  assert(strcmp(loaded.room, "coil-yard") == 0);
  assert(loaded.morality == 3);

  char record[640];
  snprintf(record, sizeof(record), "%s/ferrite_17.json", store.root);
  assert(remove(record) == 0);
  assert(rmdir(store.root) == 0);
  assert(rmdir(root) == 0);
}

int main(void) {
  test_world();
  test_event();
  test_store();
  puts("core tests passed");
  return 0;
}
