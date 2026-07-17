#include "hg_world.h"

#include <string.h>

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
         "here to quiet the pull of what they remember. Some pay for a drink. "
         "Some pay to stop remembering why.",
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
         "conduit, and a glow-rat worries at the insulation. A flooded shaft "
         "drops deeper.",
     .exits = {{"up", "nexus"}, {"down", "sump"}},
     .exit_count = 2,
     .mobs = {{"rat", "glow-rat", 12, 12}},
     .mob_count = 1},
    {.id = "sump",
     .name = "The Ferrite Sump",
     .description =
         "Black water turns orange where it meets exposed iron. The shaft "
         "continues down into machinery not yet mapped.",
     .exits = {{"up", "tunnels"}},
     .exit_count = 1},
    {.id = "dunes",
     .name = "The Black Powder Flats",
     .description =
         "Ferrite dust combs itself into ridges under a field too weak to "
         "measure and too persistent to ignore. The roof lies south.",
     .outdoors = 1,
     .exits = {{"south", "roof"}},
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
