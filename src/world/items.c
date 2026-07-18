#include "hg_items.h"

#include <stddef.h>
#include <string.h>

static const hg_item items[] = {
    {.id = "shiv",
     .name = "a rusted shiv",
     .description = "Sharp enough, if the tetanus does not get you first.",
     .slot = "weapon",
     .damage = 3,
     .value = 5},
    {.id = "rebar",
     .name = "a length of rebar",
     .description = "A meter of rusted reinforcing bar. Crude and heavy.",
     .slot = "weapon",
     .damage = 6,
     .value = 10},
    {.id = "helm",
     .name = "a dented scrap helm",
     .description = "A welded pot that has taken worse hits than you have.",
     .slot = "head",
     .armor = 1,
     .value = 6},
    {.id = "plating",
     .name = "a sheet of scrap plating",
     .description = "Buckled salvage. Heavy, dull, and just about wearable.",
     .slot = "body",
     .armor = 2,
     .value = 3},
    {.id = "charm",
     .name = "an elven charm",
     .description =
         "A woven token of knotted grass and wire, pressed into your hand by "
         "grateful refugees."},
    {.id = "radcell",
     .name = "a rad-cell",
     .description = "A cracked power cell, still warm.",
     .value = 12},
    {.id = "antidote",
     .name = "a vial of antivenom",
     .description =
         "Pressed into your hands by someone you cut free. For the poison that "
         "haunts these wastes.",
     .value = 0},
    {.id = "dust",
     .name = "a pinch of dust",
     .description = "Fine grey powder. The tavern sells it; the Grid remembers.",
     .value = 2},
};

const hg_item *hg_item_by_id(const char *id) {
  if (id == NULL) {
    return NULL;
  }
  size_t count = sizeof(items) / sizeof(items[0]);
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(items[i].id, id) == 0) {
      return &items[i];
    }
  }
  return NULL;
}

const char *hg_item_name(const char *id) {
  const hg_item *item = hg_item_by_id(id);
  return item != NULL ? item->name : id;
}
