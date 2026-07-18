#ifndef HG_ITEMS_H
#define HG_ITEMS_H

typedef struct {
  const char *id;
  const char *name;
  const char *description;
  const char *slot;
  int damage;
  int armor;
  int value;
} hg_item;

const hg_item *hg_item_by_id(const char *id);
const char *hg_item_name(const char *id);

#endif
