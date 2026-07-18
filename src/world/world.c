#include "hg_world.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const hg_room rooms[] = {
    {.id = "nexus",
     .name = "The Ferrite Nexus",
     .description =
         "A junction of cracked ceramic and black iron. Dead coils ring the "
         "walls. Each holds a little of the last field that passed through it. "
         "A market lies north, a workshop east, a bar west, and a service "
         "hatch opens underfoot.",
     .exits = {{"north", "market"},
               {"east", "workshop"},
               {"down", "tunnels"},
               {"west", "tavern"}},
     .exit_count = 4},
    {.id = "tavern",
     .name = "The Degaussed Cup",
     .description =
         "A low bar hammered together from transformer housings. People come "
         "here to quiet the pull of what they remember.",
     .exits = {{"east", "nexus"}},
     .exit_count = 1},
    {.id = "market",
     .name = "The Induction Market",
     .description =
         "Salvage hangs from wire loops and turns without wind. A Cinder Front "
         "recruiter stands on a ferrite crate, promising order, coin, and a "
         "registry that decides who counts. A reinforced door lies north.",
     .exits = {{"south", "nexus"}, {"north", "holding_pit"}},
     .exit_count = 2,
     .actions = {{"join", "take the Front's coin and help sort the living",
                  "moral", "corrupt"},
                 {"defend", "stand with the people the Front would erase",
                  "moral", "virtuous"}},
     .action_count = 2},
    {.id = "holding_pit",
     .name = "The Holding Pit",
     .description =
         "A sunken cell under a humming busbar. Names have been scratched into "
         "the walls wherever the guards failed to paint fast enough.",
     .exits = {{"south", "market"}},
     .exit_count = 1},
    {.id = "workshop",
     .name = "The Coil Winder",
     .description =
         "Copper wire, cracked varnish, and hand tools cover every bench. A "
         "tinker repairs what can still carry current. A ladder climbs to the "
         "roof.",
     .exits = {{"west", "nexus"}, {"up", "roof"}},
     .exit_count = 2},
    {.id = "roof",
     .name = "The Lamination Roof",
     .description =
         "Thin steel plates shiver under a grey sky. The wastes open north. To "
         "the west, a gantry crosses into stacked black cores and the Coil "
         "Yard.",
     .outdoors = 1,
     .exits = {{"down", "workshop"}, {"north", "dunes"}, {"west", "coil-yard"}},
     .exit_count = 3},
    {.id = "tunnels",
     .name = "Service Tunnels",
     .description =
         "Cable trenches run beneath the Nexus. Rust water gathers around old "
         "conduit. A flooded shaft drops deeper.",
     .exits = {{"up", "nexus"}, {"down", "sump"}},
     .exit_count = 2},
    {.id = "sump",
     .name = "The Ferrite Sump",
     .description =
         "Black water turns orange where it meets exposed iron. The shaft "
         "continues down into submerged machinery.",
     .exits = {{"up", "tunnels"}, {"down", "floodgate"}},
     .exit_count = 2},
    {.id = "floodgate",
     .name = "The Floodgate",
     .description =
         "A sealed bulkhead groans under pressure. A stranded operator waits "
         "beside a dead console, watching the cold storage racks beyond.",
     .exits = {{"up", "sump"}, {"north", "coldrow"}},
     .exit_count = 2},
    {.id = "coldrow",
     .name = "Cold Storage Row",
     .description =
         "Racks of dead drives sweat condensation. Something pale moves between "
         "the cabinets, feeding on residual current.",
     .exits = {{"south", "floodgate"}},
     .exit_count = 1},
    {.id = "dunes",
     .name = "The Black Powder Flats",
     .description =
         "Ferrite dust combs itself into ridges under a field too weak to "
         "measure and too persistent to ignore. The roof lies south; Scorch "
         "Road runs east.",
     .outdoors = 1,
     .exits = {{"south", "roof"},
               {"east", "scorch_road"},
               {"north", "checkpoint"}},
     .exit_count = 3},
    {.id = "scorch_road",
     .name = "Scorch Road",
     .description =
         "A highway the sun has been working on for a long time. Heat-shimmer "
         "crawls off the tar. The flats lie west; a waystation flag snaps to "
         "the east; an abandoned transit hub opens south.",
     .outdoors = 1,
     .exits = {{"west", "dunes"},
               {"east", "waystation"},
               {"south", "transit_hub"}},
     .exit_count = 3},
    {.id = "waystation",
     .name = "Refugee Waystation",
     .description =
         "A huddle of tarps and water-drums where free folk catch their breath. "
         "Eyes track every newcomer, weighing which side they came in on.",
     .outdoors = 1,
     .exits = {{"west", "scorch_road"}},
     .exit_count = 1},
    {.id = "transit_hub",
     .name = "The Old Transit Hub",
     .description =
         "Collapsed platforms and silent rails. Distress banners still hang "
         "from rusted girders.",
     .exits = {{"north", "scorch_road"}},
     .exit_count = 1},
    {.id = "checkpoint",
     .name = "The Ash Checkpoint",
     .description =
         "The Cinder Front's first wall. Ash-grey troopers watch the road and "
         "the people on it.",
     .outdoors = 1,
     .exits = {{"south", "dunes"}, {"north", "gate"}},
     .exit_count = 2},
    {.id = "gate",
     .name = "The Stronghold Gate",
     .description =
         "Reinforced plating and welded scrap form a throat into the Front's "
         "yard. Beyond, drills and orders carry on the wind.",
     .exits = {{"south", "checkpoint"}, {"north", "muster"}},
     .exit_count = 2},
    {.id = "muster",
     .name = "The Muster Yard",
     .description =
         "Drilled Front troopers form and reform under ash banners. Cages sit "
         "to the west; the war room waits north.",
     .exits = {{"south", "gate"}, {"west", "cells"}, {"north", "warroom"}},
     .exit_count = 3},
    {.id = "cells",
     .name = "The Holding Cells",
     .description =
         "Iron cages bolted into concrete. Some are empty. Some are waiting.",
     .exits = {{"east", "muster"}},
     .exit_count = 1},
    {.id = "warroom",
     .name = "The War Room",
     .description =
         "Maps of the wastes cover a steel table. The dais opens above.",
     .exits = {{"south", "muster"}, {"up", "dais"}},
     .exit_count = 2},
    {.id = "dais",
     .name = "The Ashmonger's Dais",
     .description =
         "A raised platform of welded scrap and ash. The commander of the "
         "Cinder Front holds court here.",
     .exits = {{"down", "warroom"}},
     .exit_count = 1},
    {.id = "coil-yard",
     .name = "The Coil Yard",
     .description =
         "Ferrite cores stand in ranks around induction rigs that have been "
         "dead for years. Exposed metal still creeps toward them. The roof is "
         "east; a service road runs north through the stacks.",
     .exits = {{"east", "roof"}, {"north", "bias-road"}},
     .exit_count = 2},
    {.id = "bias-road",
     .name = "Bias Road",
     .description =
         "Alternating coils line a road built to set the shape of anything "
         "carried through. Front firelight burns north. West, a degaussing bay "
         "keeps failing to erase what was left inside.",
     .outdoors = 1,
     .exits = {{"south", "coil-yard"},
               {"north", "bias-checkpoint"},
               {"west", "remanence-bay"}},
     .exit_count = 3},
    {.id = "bias-checkpoint",
     .name = "The Bias Checkpoint",
     .description =
         "The Cinder Front has stretched a gate across the road. Refugees wait "
         "beside bundles opened for inspection. A captain decides whose papers "
         "prove a life and whose absence proves guilt.",
     .outdoors = 1,
     .exits = {{"south", "bias-road"}},
     .exit_count = 1,
     .actions = {{"defend", "put yourself between the Front and the refugees",
                  "moral", "virtuous"},
                 {"join", "take the captain's coin and work the line", "moral",
                  "corrupt"}},
     .action_count = 2},
    {.id = "remanence-bay",
     .name = "The Remanence Bay",
     .description =
         "A failed degaussing chamber pulses without effect. Names cut into "
         "its shielding return after every sweep. Someone wanted testimony "
         "reduced to noise. It held.",
     .exits = {{"east", "bias-road"}},
     .exit_count = 1,
     .actions = {{"witness", "speak the names the sweep could not erase",
                  "moral", "virtuous"}},
     .action_count = 1},
};

static void seed_mob(hg_world_state *state, const char *id, const char *name,
                     const char *description, const char *room, int max_hp,
                     int damage, int xp, int respawn_ms) {
  if (state->mob_count >= HG_MAX_LIVE_MOBS) {
    return;
  }
  hg_live_mob *mob = &state->mobs[state->mob_count++];
  memset(mob, 0, sizeof(*mob));
  snprintf(mob->id, sizeof(mob->id), "%s", id);
  snprintf(mob->name, sizeof(mob->name), "%s", name);
  snprintf(mob->description, sizeof(mob->description), "%s", description);
  snprintf(mob->room, sizeof(mob->room), "%s", room);
  mob->hp = max_hp;
  mob->max_hp = max_hp;
  mob->damage = damage;
  mob->xp = xp;
  mob->alive = 1;
  mob->respawn_ms = respawn_ms;
}

void hg_world_init(hg_world_state *state) {
  memset(state, 0, sizeof(*state));
  state->started = time(NULL);
  seed_mob(state, "rat", "a glow-rat",
           "A bloated rodent, fur matted and faintly luminous with absorbed "
           "rads.",
           "tunnels", 12, 4, 8, 20000);
  seed_mob(state, "raider", "a wastes raider",
           "A scarred figure wrapped in sun-bleached rags and scavenged plate.",
           "scorch_road", 22, 6, 20, 40000);
  seed_mob(state, "warden", "the warden",
           "A chrome-masked jailer, broad as a doorway.", "holding_pit", 18, 5,
           40, 60000);
  seed_mob(state, "leech", "a data-leech",
           "A pale, boneless thing clamped to a live rack, swollen with stolen "
           "current.",
           "coldrow", 18, 5, 16, 30000);
  seed_mob(state, "trooper", "a Cinder Front trooper",
           "A drilled Front soldier in matched ash-grey gear.", "muster", 30, 6,
           28, 60000);
  seed_mob(state, "ashmonger", "the Ashmonger",
           "The commander of the Cinder Front, ash-crowned and certain.",
           "dais", 80, 12, 200, 0);
}

const hg_room *hg_world_start(void) { return hg_world_room("nexus"); }

const hg_room *hg_world_room(const char *id) {
  size_t count = sizeof(rooms) / sizeof(rooms[0]);
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(rooms[i].id, id) == 0) {
      return &rooms[i];
    }
  }
  return NULL;
}

const hg_room *hg_world_move(const hg_room *from, const char *direction) {
  if (from == NULL || direction == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < from->exit_count; ++i) {
    if (strcmp(from->exits[i].direction, direction) == 0) {
      return hg_world_room(from->exits[i].destination);
    }
  }
  return NULL;
}

hg_live_mob *hg_world_mob_in_room(hg_world_state *state, const char *room_id,
                                  const char *mob_id) {
  if (state == NULL || room_id == NULL || mob_id == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < state->mob_count; ++i) {
    hg_live_mob *mob = &state->mobs[i];
    if (mob->alive && strcmp(mob->room, room_id) == 0 &&
        strcasecmp(mob->id, mob_id) == 0) {
      return mob;
    }
  }
  return NULL;
}

size_t hg_world_mobs_in_room(hg_world_state *state, const char *room_id,
                             hg_live_mob **out, size_t out_cap) {
  size_t found = 0;
  if (state == NULL || room_id == NULL) {
    return 0;
  }
  for (size_t i = 0; i < state->mob_count && found < out_cap; ++i) {
    hg_live_mob *mob = &state->mobs[i];
    if (mob->alive && strcmp(mob->room, room_id) == 0) {
      out[found++] = mob;
    }
  }
  return found;
}

void hg_world_tick_respawns(hg_world_state *state) {
  if (state == NULL) {
    return;
  }
  time_t now = time(NULL);
  for (size_t i = 0; i < state->mob_count; ++i) {
    hg_live_mob *mob = &state->mobs[i];
    if (mob->alive || mob->respawn_ms <= 0 || mob->died_at == 0) {
      continue;
    }
    if ((now - mob->died_at) * 1000 >= mob->respawn_ms) {
      mob->hp = mob->max_hp;
      mob->alive = 1;
      mob->died_at = 0;
    }
  }
}
