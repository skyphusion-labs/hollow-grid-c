#ifndef HG_GRID_H
#define HG_GRID_H

#include <stddef.h>

typedef struct hg_grid hg_grid;

typedef struct {
  int level;
  int xp;
  int gold;
  int morality;
  char faction[16];
  char title[48];
  char race[16];
  int ashsworn;
} hg_char_sheet;

typedef struct {
  int id;
  char world[48];
  char sender[33];
  char text[240];
} hg_grid_cast;

typedef struct {
  char id[48];
  char url[160];
  long long last_seen_ms;
} hg_grid_world;

typedef struct {
  char world[48];
  char name[33];
  char regard[48];
  char title[48];
  long long at_ms;
} hg_grid_presence;

typedef struct {
  char world[48];
  char name[33];
  char room[32];
  long long at_ms;
} hg_grid_fallen;

typedef struct {
  char kind[32];
  int count;
} hg_grid_ledger_kind;

/* Returns NULL when hub_url is empty/NULL (caller stays on LocalHub). */
hg_grid *hg_grid_open(const char *hub_url, const char *token);
void hg_grid_close(hg_grid *grid);

int hg_grid_ping(hg_grid *grid, long *latency_ms);

int hg_grid_register(hg_grid *grid, const char *world, const char *url);
int hg_grid_shift_tide(hg_grid *grid, int delta, int *out_tide);
int hg_grid_tide(hg_grid *grid, int *out_tide);

int hg_grid_gridcast(hg_grid *grid, const char *world, const char *sender,
                     const char *text);
int hg_grid_casts_since(hg_grid *grid, int since_id, int limit,
                        hg_grid_cast *out, size_t cap, size_t *out_count);

int hg_grid_list_worlds(hg_grid *grid, hg_grid_world *out, size_t cap,
                        size_t *out_count);
int hg_grid_report_presence(hg_grid *grid, const char *world,
                            const hg_grid_presence *entries, size_t count,
                            long long at_ms);
int hg_grid_fetch_presence(hg_grid *grid, long long max_age_ms,
                           hg_grid_presence *out, size_t cap,
                           size_t *out_count);

int hg_grid_load_character(hg_grid *grid, const char *name, hg_char_sheet *out,
                           int *found);
int hg_grid_commit_character(hg_grid *grid, const char *name,
                             const hg_char_sheet *sheet);

int hg_grid_record_fallen(hg_grid *grid, const char *world, const char *name,
                          const char *room, long long at_ms);
int hg_grid_recent_fallen(hg_grid *grid, int limit, hg_grid_fallen *out,
                          size_t cap, size_t *out_count);

int hg_grid_ledger_stats(hg_grid *grid, hg_grid_ledger_kind *out, size_t cap,
                         size_t *out_count, int *out_total);
int hg_grid_prune_ledger(hg_grid *grid, int *removed);

#endif
