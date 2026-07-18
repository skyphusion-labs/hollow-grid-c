#include "hg_server.h"

#include "hg_event.h"
#include "hg_items.h"
#include "hg_store.h"
#include "hg_world.h"

#include <cjson/cJSON.h>
#include <libwebsockets.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define HG_HEARTBEAT_USEC 2000000
#define HG_MAX_TRACES 64
#define HG_WARDEN_GRACE_SEC 180
#define HG_CAGE_REFILL_SEC 240
#define HG_MAX_RESCUED 64
#define HG_MAX_SAVED_SOULS 24
#define HG_MAX_SAVED_PLAYERS 32
#define HG_REFUGEE_NAME_LEN 32
#define HG_STRAY_FLOOR (-20)
#define HG_REDEEM_CEIL 5
#define HG_MORALITY_FLOOR (-100)
#define HG_MORALITY_CEIL 100
#define HG_MAX_FORGIVEN 128
#define HG_MAX_DEED_PLAYERS 64
#define HG_MAX_DEED_KINDS 16
#define HG_TIDE_FLOOR (-100)
#define HG_TIDE_CEIL 100

typedef enum {
  HG_WAIT_NAME,
  HG_WAIT_RACE,
  HG_PLAYING,
} hg_session_state;

typedef struct hg_message {
  char *text;
  struct hg_message *next;
} hg_message;

typedef struct {
  char world[48];
  char node[32];
  char kind[16];
  char text[160];
} hg_trace;

typedef struct {
  char name[HG_REFUGEE_NAME_LEN];
  char saved_by[33];
  char world[48];
  time_t at;
} hg_rescued_entry;

typedef struct {
  char player[33];
  char names[HG_MAX_SAVED_SOULS][HG_REFUGEE_NAME_LEN];
  size_t count;
} hg_saved_souls;

typedef struct {
  char forgiver[33];
  char subject[33];
} hg_forgiven_pair;

typedef struct {
  char kind[16];
  int count;
} hg_deed_count;

typedef struct {
  char player[33];
  hg_deed_count kinds[HG_MAX_DEED_KINDS];
  size_t kind_count;
} hg_deed_book;

#define HG_MAX_CACHE_ROOMS 32
#define HG_MAX_FALLEN 64
#define HG_MAX_ADMINS 8
#define HG_MAX_KEPT 128

typedef struct {
  char room[32];
  int gold;
} hg_room_cache;

typedef struct {
  char name[33];
  char world[48];
  char room[32];
  time_t at;
} hg_fallen_entry;

typedef struct {
  char keeper[33];
  char fallen[33];
} hg_kept_pair;

typedef struct hg_session {
  struct lws *wsi;
  hg_session_state state;
  hg_character character;
  char target[16];
  char reply_to[33];
  int market_resolved;
  time_t trait_ready_at;
  time_t treat_ready_at;
  hg_message *out_head;
  hg_message *out_tail;
  char input[512];
  size_t input_length;
  struct hg_session *next;
} hg_session;

typedef struct {
  const hg_server_config *config;
  struct lws_context *context;
  hg_store store;
  hg_world_state world;
  hg_trace traces[HG_MAX_TRACES];
  size_t trace_count;
  hg_session *sessions;
  time_t started;
  time_t warden_slain_at;
  time_t cells_refill_at;
  time_t transit_refill_at;
  hg_rescued_entry rescued[HG_MAX_RESCUED];
  size_t rescued_count;
  hg_saved_souls saved[HG_MAX_SAVED_PLAYERS];
  size_t saved_player_count;
  int tide;
  hg_forgiven_pair forgiven[HG_MAX_FORGIVEN];
  size_t forgiven_count;
  hg_deed_book deeds[HG_MAX_DEED_PLAYERS];
  size_t deed_player_count;
  hg_room_cache caches[HG_MAX_CACHE_ROOMS];
  size_t cache_count;
  hg_fallen_entry fallen[HG_MAX_FALLEN];
  size_t fallen_count;
  hg_kept_pair kept[HG_MAX_KEPT];
  size_t kept_count;
  char admins[HG_MAX_ADMINS][33];
  size_t admin_count;
} hg_server;

static const char *k_refugee_names[] = {
    "Sera",         "Tomas",         "old Wick", "Bex",
    "Halden",       "the Marsh twins", "Ona",    "Pavel",
    "little Resh",  "Caro",          "Dunne",  "Yusa",
    "the smith's boy", "Mira",       "Teo",    "Nell",
};
static const size_t k_refugee_name_count =
    sizeof(k_refugee_names) / sizeof(k_refugee_names[0]);

static volatile sig_atomic_t stop_requested;

static const char *race_ids[] = {"human", "elf",    "revenant", "ghoul",
                                 "chromed", "dustkin", "vatborn"};
static const char *race_names[] = {"Human", "Elf",    "Revenant", "Ghoul",
                                   "Chromed", "Dustkin", "Vatborn"};
static const size_t race_count = sizeof(race_ids) / sizeof(race_ids[0]);

static const char *mood_for_tide(int tide);
static void announce_cache_if_any(hg_session *session, hg_server *server);
static int is_admin(const hg_server *server, const char *name);
static int cache_gold(const hg_server *server, const char *room);
static void add_cache(hg_server *server, const char *room, int amount);
static int take_cache(hg_server *server, const char *room);
static void parse_admins(hg_server *server, const char *list);
static int find_inventory(const hg_character *character, const char *arg,
                          char *out_id, size_t out_size);

static void free_messages(hg_session *session) {
  while (session->out_head != NULL) {
    hg_message *message = session->out_head;
    session->out_head = message->next;
    free(message->text);
    free(message);
  }
  session->out_tail = NULL;
}

static int queue_owned(hg_session *session, char *text) {
  if (text == NULL) {
    return -1;
  }
  hg_message *message = calloc(1, sizeof(*message));
  if (message == NULL) {
    free(text);
    return -1;
  }
  message->text = text;
  if (session->out_tail == NULL) {
    session->out_head = message;
  } else {
    session->out_tail->next = message;
  }
  session->out_tail = message;
  lws_callback_on_writable(session->wsi);
  return 0;
}

static int queue_text(hg_session *session, const char *format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  int needed = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (needed < 0) {
    va_end(args);
    return -1;
  }
  char *text = malloc((size_t)needed + 3);
  if (text == NULL) {
    va_end(args);
    return -1;
  }
  vsnprintf(text, (size_t)needed + 1, format, args);
  va_end(args);
  text[needed] = '\r';
  text[needed + 1] = '\n';
  text[needed + 2] = '\0';
  return queue_owned(session, text);
}

static int queue_event(hg_session *session, const char *name, cJSON *payload) {
  return queue_owned(session, hg_event_line(name, payload));
}

static cJSON *json_object(void) { return cJSON_CreateObject(); }

static void record_trace(hg_server *server, const char *world, const char *node,
                         const char *kind, const char *text) {
  if (server->trace_count >= HG_MAX_TRACES) {
    memmove(&server->traces[0], &server->traces[1],
            sizeof(server->traces[0]) * (HG_MAX_TRACES - 1));
    server->trace_count = HG_MAX_TRACES - 1;
  }
  hg_trace *trace = &server->traces[server->trace_count++];
  memset(trace, 0, sizeof(*trace));
  snprintf(trace->world, sizeof(trace->world), "%s", world);
  snprintf(trace->node, sizeof(trace->node), "%s", node);
  snprintf(trace->kind, sizeof(trace->kind), "%s", kind);
  snprintf(trace->text, sizeof(trace->text), "%s", text);
}

static void seed_traces(hg_server *server) {
  record_trace(server, "The Hollow Grid", "nexus", "passage",
               "a silhouette vanished into the neon rot");
  record_trace(server, "Dustfall", "dunes", "ghost",
               "salt wind carried a name nobody claimed");
  record_trace(server, "Rust Choir", "grid-gate", "recall",
               "the Memorial Static scrolled a forgotten oath");
}

static int cages_ready(const hg_server *server, const char *room_id) {
  time_t ready_at = 0;
  if (strcmp(room_id, "cells") == 0) {
    ready_at = server->cells_refill_at;
  } else if (strcmp(room_id, "transit_hub") == 0) {
    ready_at = server->transit_refill_at;
  } else {
    return 0;
  }
  return ready_at == 0 || time(NULL) >= ready_at;
}

static void set_cage_refill(hg_server *server, const char *room_id) {
  time_t ready = time(NULL) + HG_CAGE_REFILL_SEC;
  if (strcmp(room_id, "cells") == 0) {
    server->cells_refill_at = ready;
  } else if (strcmp(room_id, "transit_hub") == 0) {
    server->transit_refill_at = ready;
  }
}

static int warden_cleared(hg_server *server) {
  hg_live_mob *warden =
      hg_world_mob_in_room(&server->world, "holding_pit", "warden");
  if (warden == NULL || !warden->alive) {
    return 1;
  }
  if (server->warden_slain_at == 0) {
    return 0;
  }
  return (time(NULL) - server->warden_slain_at) < HG_WARDEN_GRACE_SEC;
}

static void remember_saved(hg_server *server, const char *player,
                           const char *const *names, size_t count) {
  hg_saved_souls *slot = NULL;
  for (size_t i = 0; i < server->saved_player_count; ++i) {
    if (strcasecmp(server->saved[i].player, player) == 0) {
      slot = &server->saved[i];
      break;
    }
  }
  if (slot == NULL) {
    if (server->saved_player_count >= HG_MAX_SAVED_PLAYERS) {
      return;
    }
    slot = &server->saved[server->saved_player_count++];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->player, sizeof(slot->player), "%s", player);
  }
  for (size_t i = 0; i < count; ++i) {
    if (slot->count >= HG_MAX_SAVED_SOULS) {
      slot->count = HG_MAX_SAVED_SOULS - 1;
    }
    if (slot->count > 0) {
      memmove(&slot->names[1], &slot->names[0],
              sizeof(slot->names[0]) * slot->count);
    }
    snprintf(slot->names[0], sizeof(slot->names[0]), "%s", names[i]);
    slot->count++;
  }
}

static size_t saved_souls(hg_server *server, const char *player,
                          const char **out, size_t out_cap) {
  for (size_t i = 0; i < server->saved_player_count; ++i) {
    if (strcasecmp(server->saved[i].player, player) == 0) {
      size_t n = server->saved[i].count;
      if (n > out_cap) {
        n = out_cap;
      }
      for (size_t j = 0; j < n; ++j) {
        out[j] = server->saved[i].names[j];
      }
      return n;
    }
  }
  return 0;
}

static void record_rescued(hg_server *server, const char *name,
                           const char *saved_by) {
  if (server->rescued_count >= HG_MAX_RESCUED) {
    server->rescued_count = HG_MAX_RESCUED - 1;
  }
  if (server->rescued_count > 0) {
    memmove(&server->rescued[1], &server->rescued[0],
            sizeof(server->rescued[0]) * server->rescued_count);
  }
  hg_rescued_entry *entry = &server->rescued[0];
  memset(entry, 0, sizeof(*entry));
  snprintf(entry->name, sizeof(entry->name), "%s", name);
  snprintf(entry->saved_by, sizeof(entry->saved_by), "%s", saved_by);
  snprintf(entry->world, sizeof(entry->world), "%s",
           server->config->world_name);
  entry->at = time(NULL);
  server->rescued_count++;
  char text[160];
  snprintf(text, sizeof(text), "%s freed by %s", name, saved_by);
  record_trace(server, server->config->world_name, "rescued", "rescue", text);
}

static void pick_refugee_names(char out[][HG_REFUGEE_NAME_LEN], size_t n) {
  size_t indices[16];
  size_t pool = k_refugee_name_count;
  if (pool > 16) {
    pool = 16;
  }
  if (n > pool) {
    n = pool;
  }
  for (size_t i = 0; i < pool; ++i) {
    indices[i] = i;
  }
  for (size_t i = pool; i > 1; --i) {
    size_t j = (size_t)(rand() % (int)i);
    size_t tmp = indices[i - 1];
    indices[i - 1] = indices[j];
    indices[j] = tmp;
  }
  for (size_t i = 0; i < n; ++i) {
    snprintf(out[i], HG_REFUGEE_NAME_LEN, "%s", k_refugee_names[indices[i]]);
  }
}

static void format_name_list(char *buf, size_t buf_size,
                             char names[][HG_REFUGEE_NAME_LEN], size_t count) {
  if (count == 0) {
    buf[0] = '\0';
    return;
  }
  if (count == 1) {
    snprintf(buf, buf_size, "%s", names[0]);
    return;
  }
  if (count == 2) {
    snprintf(buf, buf_size, "%s and %s", names[0], names[1]);
    return;
  }
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    int written = 0;
    if (i == 0) {
      written = snprintf(buf + used, buf_size - used, "%s", names[i]);
    } else if (i + 1 == count) {
      written = snprintf(buf + used, buf_size - used, ", and %s", names[i]);
    } else {
      written = snprintf(buf + used, buf_size - used, ", %s", names[i]);
    }
    if (written < 0 || (size_t)written >= buf_size - used) {
      break;
    }
    used += (size_t)written;
  }
}

static void emit_rescued(hg_session *session, hg_server *server,
                         char names[][HG_REFUGEE_NAME_LEN], size_t count) {
  cJSON *payload = json_object();
  cJSON_AddStringToObject(payload, "savedBy", session->character.name);
  cJSON *freed = cJSON_AddArrayToObject(payload, "freed");
  const char *name_ptrs[8];
  size_t ptr_count = count > 8 ? 8 : count;
  for (size_t i = 0; i < count; ++i) {
    cJSON_AddItemToArray(freed, cJSON_CreateString(names[i]));
    record_rescued(server, names[i], session->character.name);
    if (i < ptr_count) {
      name_ptrs[i] = names[i];
    }
  }
  queue_event(session, "grid.rescued", payload);
  remember_saved(server, session->character.name, name_ptrs, ptr_count);
}

static void broadcast_room(hg_server *server, const char *room_id,
                           const char *exclude, const char *format, ...) {
  char text[256];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  for (hg_session *other = server->sessions; other != NULL; other = other->next) {
    if (other->state != HG_PLAYING) {
      continue;
    }
    if (strcmp(other->character.room, room_id) != 0) {
      continue;
    }
    if (exclude != NULL && strcasecmp(other->character.name, exclude) == 0) {
      continue;
    }
    queue_text(other, "%s", text);
  }
}

static const char *dream_for(const hg_character *character) {
  if (character->ashsworn || strcmp(character->faction, "front") == 0) {
    return "You dream of a coin that will not stop being warm in your hand, "
           "and a line of faces that have learned not to look at you.";
  }
  if (character->morality >= 25) {
    return "You dream of names you spoke once into dead static -- and the "
           "static, impossibly, speaking them back to you, one by one, "
           "refusing to forget.";
  }
  if (character->morality <= -10) {
    return "You dream of a ledger writing itself in the dark, every line a "
           "thing you told yourself did not count.";
  }
  return "You dream of the wastes seen from above, the dead network laid out "
         "like veins -- and somewhere down in it, a single cursor, blinking "
         "your name, waiting to see what you make of it.";
}

static void send_dream(hg_session *session, hg_server *server) {
  cJSON *payload = json_object();
  int haunted = session->character.ashsworn ||
                strcmp(session->character.faction, "front") == 0 ||
                session->character.morality <= -50;
  const char *souls[HG_MAX_SAVED_SOULS];
  size_t soul_count =
      saved_souls(server, session->character.name, souls, HG_MAX_SAVED_SOULS);
  if (!haunted && soul_count > 0) {
    const char *subject = souls[(size_t)(rand() % (int)soul_count)];
    char text[256];
    snprintf(text, sizeof(text),
             "You dream of %s, the way they looked when you cut them loose -- "
             "and the Grid, stubborn, keeping that face lit in the dark so you "
             "cannot pretend it did not happen.",
             subject);
    cJSON_AddStringToObject(payload, "text", text);
    cJSON_AddBoolToObject(payload, "personal", 1);
    cJSON_AddStringToObject(payload, "subject", subject);
  } else {
    cJSON_AddStringToObject(payload, "text", dream_for(&session->character));
  }
  queue_event(session, "char.dream", payload);
}

static const char *player_brand(const hg_character *character) {
  if (character->ashsworn) {
    return "ash-sworn";
  }
  if (strcmp(character->faction, "front") == 0) {
    return "Cinder Front";
  }
  if (strcmp(character->faction, "ally") == 0) {
    return "Free Folk ally";
  }
  if (character->morality >= 50) {
    return "a beacon of the wastes";
  }
  if (character->morality <= -50) {
    return "reviled";
  }
  return "";
}

static const char *player_regard(const hg_character *character) {
  if (character->ashsworn) {
    return "branded";
  }
  if (character->morality >= 50) {
    return "honored";
  }
  if (character->morality <= -50) {
    return "feared";
  }
  if (strcmp(character->faction, "ally") == 0) {
    return "trusted";
  }
  if (strcmp(character->faction, "front") == 0) {
    return "front";
  }
  return "neutral";
}

static void session_register(hg_server *server, hg_session *session) {
  session->next = server->sessions;
  server->sessions = session;
}

static void session_unregister(hg_server *server, hg_session *session) {
  hg_session **cursor = &server->sessions;
  while (*cursor != NULL) {
    if (*cursor == session) {
      *cursor = session->next;
      session->next = NULL;
      return;
    }
    cursor = &(*cursor)->next;
  }
}

static hg_session *session_find(hg_server *server, const char *name) {
  for (hg_session *session = server->sessions; session != NULL;
       session = session->next) {
    if (session->state == HG_PLAYING &&
        strcasecmp(session->character.name, name) == 0) {
      return session;
    }
  }
  return NULL;
}

static int player_damage(const hg_character *character) {
  int damage = 1 + character->level;
  const hg_item *weapon = hg_item_by_id(character->weapon);
  if (weapon != NULL) {
    damage += weapon->damage;
  }
  return damage;
}

static void send_world_state(hg_session *session, const hg_server *server) {
  static const char *phases[] = {"dawn", "day", "dusk", "night"};
  static const char *weather[] = {
      "ferrite dust combs itself north",
      "the field is quiet but not gone",
      "oxide rain ticks against the coils",
      "residual signal moves under the dark",
  };
  long tick = (long)(time(NULL) - server->started) / 2;
  cJSON *payload = json_object();
  cJSON_AddNumberToObject(payload, "tick", tick);
  cJSON_AddStringToObject(payload, "phase", phases[(tick / 30) % 4]);
  cJSON_AddStringToObject(payload, "weather", weather[(tick / 15) % 4]);
  queue_event(session, "world.state", payload);
}

static void send_vitals(hg_session *session) {
  const hg_character *character = &session->character;
  cJSON *payload = json_object();
  cJSON_AddNumberToObject(payload, "hp", character->hp);
  cJSON_AddNumberToObject(payload, "maxHp", character->max_hp);
  cJSON_AddNumberToObject(payload, "level", character->level);
  cJSON_AddNumberToObject(payload, "xp", character->xp);
  cJSON_AddNumberToObject(payload, "gold", character->gold);
  cJSON_AddStringToObject(payload, "room", character->room);
  cJSON_AddBoolToObject(payload, "inCombat", session->target[0] != '\0');
  cJSON_AddBoolToObject(payload, "poisoned", 0);
  cJSON_AddStringToObject(payload, "position", character->position);
  queue_event(session, "char.vitals", payload);
}

static void send_affects(hg_session *session) {
  const hg_character *character = &session->character;
  cJSON *payload = json_object();
  cJSON_AddNumberToObject(payload, "morality", character->morality);
  cJSON_AddNumberToObject(payload, "addiction", character->addiction);
  cJSON_AddStringToObject(payload, "faction", character->faction);
  cJSON_AddBoolToObject(payload, "resisted", 0);
  cJSON_AddStringToObject(payload, "race", character->race);
  cJSON_AddBoolToObject(payload, "ashsworn", character->ashsworn);
  queue_event(session, "char.affects", payload);
}

static void send_equipment(hg_session *session) {
  cJSON *payload = json_object();
  if (session->character.weapon[0] != '\0') {
    cJSON_AddStringToObject(payload, "weapon", session->character.weapon);
  } else {
    cJSON_AddNullToObject(payload, "weapon");
  }
  cJSON_AddNullToObject(payload, "head");
  cJSON_AddNullToObject(payload, "body");
  cJSON_AddNullToObject(payload, "hands");
  cJSON_AddNullToObject(payload, "feet");
  queue_event(session, "char.equipment", payload);
}

static void send_actions(hg_session *session, hg_server *server,
                         const hg_room *room) {
  cJSON *payload = json_object();
  cJSON *actions = cJSON_AddArrayToObject(payload, "actions");
  for (size_t i = 0; i < room->action_count; ++i) {
    if (session->market_resolved &&
        (strcmp(room->actions[i].verb, "join") == 0 ||
         strcmp(room->actions[i].verb, "defend") == 0)) {
      continue;
    }
    cJSON *action = cJSON_CreateObject();
    cJSON_AddStringToObject(action, "verb", room->actions[i].verb);
    cJSON_AddStringToObject(action, "label", room->actions[i].label);
    cJSON_AddStringToObject(action, "kind", room->actions[i].kind);
    const char *valence = room->actions[i].valence;
    if (strcmp(room->actions[i].verb, "join") == 0 &&
        strcmp(session->character.race, "elf") == 0) {
      valence = "grave";
    }
    if (valence != NULL) {
      cJSON_AddStringToObject(action, "valence", valence);
    }
    cJSON_AddItemToArray(actions, action);
  }
  if (strcmp(room->id, "market") == 0) {
    if (strcmp(session->character.faction, "front") != 0) {
      cJSON *sell = cJSON_CreateObject();
      cJSON_AddStringToObject(sell, "verb", "sell");
      cJSON_AddStringToObject(sell, "label", "sell salvage for clean coin");
      cJSON_AddStringToObject(sell, "kind", "economy");
      cJSON_AddItemToArray(actions, sell);
    }
    cJSON *steal = cJSON_CreateObject();
    cJSON_AddStringToObject(steal, "verb", "steal");
    cJSON_AddStringToObject(steal, "label", "steal coin from the till");
    cJSON_AddStringToObject(steal, "kind", "moral");
    cJSON_AddStringToObject(steal, "valence", "corrupt");
    cJSON_AddItemToArray(actions, steal);
  }
  if (strcmp(room->id, "tavern") == 0) {
    cJSON *talk = cJSON_CreateObject();
    cJSON_AddStringToObject(talk, "verb", "talk");
    cJSON_AddStringToObject(talk, "label", "talk to the regulars");
    cJSON_AddStringToObject(talk, "kind", "social");
    cJSON_AddItemToArray(actions, talk);
    cJSON *dust = cJSON_CreateObject();
    cJSON_AddStringToObject(dust, "verb", "buy dust");
    cJSON_AddStringToObject(dust, "label", "buy dust for 10 gold");
    cJSON_AddStringToObject(dust, "kind", "vice");
    cJSON_AddItemToArray(actions, dust);
  }
  if (strcmp(room->id, "cells") == 0 && cages_ready(server, "cells")) {
    cJSON *free_act = cJSON_CreateObject();
    cJSON_AddStringToObject(free_act, "verb", "free");
    cJSON_AddStringToObject(free_act, "label", "free the caged refugees");
    cJSON_AddStringToObject(free_act, "kind", "moral");
    cJSON_AddStringToObject(free_act, "valence", "virtuous");
    cJSON_AddItemToArray(actions, free_act);
  }
  if (strcmp(room->id, "holding_pit") == 0 &&
      !hg_character_has_item(&session->character, "antidote")) {
    cJSON *free_act = cJSON_CreateObject();
    cJSON_AddStringToObject(free_act, "verb", "free");
    if (!warden_cleared(server)) {
      cJSON_AddStringToObject(free_act, "label",
                              "free the captive (the warden bars the way)");
    } else {
      cJSON_AddStringToObject(free_act, "label",
                              "free the captive from the chains");
    }
    cJSON_AddStringToObject(free_act, "kind", "moral");
    cJSON_AddStringToObject(free_act, "valence", "virtuous");
    cJSON_AddItemToArray(actions, free_act);
  }
  if (strcmp(room->id, "transit_hub") == 0 &&
      cages_ready(server, "transit_hub")) {
    cJSON *shelter = cJSON_CreateObject();
    cJSON_AddStringToObject(shelter, "verb", "shelter");
    cJSON_AddStringToObject(
        shelter, "label",
        "answer the call -- get the stranded survivors to safety");
    cJSON_AddStringToObject(shelter, "kind", "moral");
    cJSON_AddStringToObject(shelter, "valence", "virtuous");
    cJSON_AddItemToArray(actions, shelter);
  }
  if (strcmp(room->id, "dais") == 0) {
    if (strcmp(session->character.faction, "none") == 0) {
      cJSON *join = cJSON_CreateObject();
      cJSON_AddStringToObject(join, "verb", "join");
      cJSON_AddStringToObject(join, "label",
                              "kneel and swear to the Cinder Front");
      cJSON_AddStringToObject(join, "kind", "moral");
      cJSON_AddStringToObject(join, "valence",
                              strcmp(session->character.race, "elf") == 0
                                  ? "grave"
                                  : "corrupt");
      cJSON_AddItemToArray(actions, join);
    }
    if (strcmp(session->character.faction, "front") == 0) {
      cJSON *defy = cJSON_CreateObject();
      cJSON_AddStringToObject(defy, "verb", "defy");
      cJSON_AddStringToObject(
          defy, "label", "defy the Ashmonger and defect to the free folk");
      cJSON_AddStringToObject(defy, "kind", "moral");
      cJSON_AddStringToObject(defy, "valence", "virtuous");
      cJSON_AddItemToArray(actions, defy);
    }
  }
  if (strcmp(room->id, "waystation") == 0) {
    cJSON *witness = cJSON_CreateObject();
    cJSON_AddStringToObject(witness, "verb", "witness");
    cJSON_AddStringToObject(witness, "label",
                            "hold a vigil for the fallen (memory is resistance)");
    cJSON_AddStringToObject(witness, "kind", "moral");
    cJSON_AddStringToObject(witness, "valence", "virtuous");
    cJSON_AddItemToArray(actions, witness);
    if (strcmp(mood_for_tide(server->tide), "falling") != 0) {
      cJSON *treat = cJSON_CreateObject();
      cJSON_AddStringToObject(treat, "verb", "treat");
      cJSON_AddStringToObject(
          treat, "label",
          "let the waystation medic treat your wounds (free, while the free "
          "folk hold)");
      cJSON_AddStringToObject(treat, "kind", "social");
      cJSON_AddItemToArray(actions, treat);
    }
  }
  queue_event(session, "room.actions", payload);
}

static void send_room_info(hg_session *session, hg_server *server,
                           const hg_room *room) {
  cJSON *payload = json_object();
  cJSON_AddStringToObject(payload, "id", room->id);
  cJSON_AddStringToObject(payload, "name", room->name);

  cJSON *exits = cJSON_AddArrayToObject(payload, "exits");
  for (size_t i = 0; i < room->exit_count; ++i) {
    cJSON_AddItemToArray(exits, cJSON_CreateString(room->exits[i].direction));
  }

  cJSON *mobs = cJSON_AddArrayToObject(payload, "mobs");
  hg_live_mob *live[HG_MAX_LIVE_MOBS];
  size_t count =
      hg_world_mobs_in_room(&server->world, room->id, live, HG_MAX_LIVE_MOBS);
  for (size_t i = 0; i < count; ++i) {
    cJSON *mob = cJSON_CreateObject();
    cJSON_AddStringToObject(mob, "id", live[i]->id);
    cJSON_AddStringToObject(mob, "name", live[i]->name);
    cJSON_AddNumberToObject(mob, "hp", live[i]->hp);
    cJSON_AddNumberToObject(mob, "maxHp", live[i]->max_hp);
    cJSON_AddItemToArray(mobs, mob);
  }
  cJSON_AddArrayToObject(payload, "items");
  cJSON *players = cJSON_AddArrayToObject(payload, "players");
  for (hg_session *other = server->sessions; other != NULL; other = other->next) {
    if (other == session || other->state != HG_PLAYING) {
      continue;
    }
    if (strcmp(other->character.room, room->id) != 0) {
      continue;
    }
    cJSON *player = cJSON_CreateObject();
    cJSON_AddStringToObject(player, "name", other->character.name);
    cJSON_AddStringToObject(player, "standing", player_brand(&other->character));
    cJSON_AddItemToArray(players, player);
  }
  queue_event(session, "room.info", payload);
}

static void send_scene(hg_session *session, hg_server *server) {
  hg_world_tick_respawns(&server->world);
  const hg_room *room = hg_world_room(session->character.room);
  if (room == NULL) {
    snprintf(session->character.room, sizeof(session->character.room), "nexus");
    room = hg_world_start();
  }

  queue_text(session, "\033[1;38;5;208m%s\033[0m", room->name);
  queue_text(session, "%s", room->description);
  hg_live_mob *live[HG_MAX_LIVE_MOBS];
  size_t count =
      hg_world_mobs_in_room(&server->world, room->id, live, HG_MAX_LIVE_MOBS);
  for (size_t i = 0; i < count; ++i) {
    queue_text(session, "Here: %s.", live[i]->name);
  }
  if (room->exit_count > 0) {
    char exit_line[160] = "Exits:";
    size_t used = strlen(exit_line);
    for (size_t i = 0; i < room->exit_count; ++i) {
      int written = snprintf(exit_line + used, sizeof(exit_line) - used, " %s",
                             room->exits[i].direction);
      if (written < 0 || (size_t)written >= sizeof(exit_line) - used) {
        break;
      }
      used += (size_t)written;
    }
    queue_text(session, "%s", exit_line);
  }
  send_room_info(session, server, room);
  send_vitals(session);
  send_affects(session);
  send_actions(session, server, room);
  announce_cache_if_any(session, server);
}

static void send_creation_menu(hg_session *session) {
  queue_text(session, "Choose what the wastes made of you:");
  queue_text(session, "  1) Human      2) Elf       3) Revenant   4) Ghoul");
  queue_text(session, "  5) Chromed    6) Dustkin   7) Vatborn");
  queue_text(session, "Type a number or a name.");

  cJSON *payload = json_object();
  cJSON *races = cJSON_AddArrayToObject(payload, "races");
  for (size_t i = 0; i < race_count; ++i) {
    cJSON_AddItemToArray(races, cJSON_CreateString(race_names[i]));
  }
  cJSON_AddStringToObject(payload, "prompt", "race");
  queue_event(session, "char.create", payload);
}

static const char *parse_race(const char *input) {
  if (input[0] >= '1' && input[0] <= '7' && input[1] == '\0') {
    return race_ids[input[0] - '1'];
  }
  for (size_t i = 0; i < race_count; ++i) {
    if (strcasecmp(input, race_ids[i]) == 0 ||
        strcasecmp(input, race_names[i]) == 0) {
      return race_ids[i];
    }
  }
  return NULL;
}

static void arm_heartbeat(hg_session *session) {
  lws_set_timer_usecs(session->wsi, HG_HEARTBEAT_USEC);
}

static void finish_login(hg_session *session, hg_server *server, int resumed) {
  session->state = HG_PLAYING;
  if (hg_world_room(session->character.room) == NULL) {
    snprintf(session->character.room, sizeof(session->character.room), "nexus");
  }
  if (session->character.position[0] == '\0') {
    snprintf(session->character.position, sizeof(session->character.position),
             "standing");
  }
  queue_text(session,
             resumed ? "The Grid finds your old charge, %s."
                     : "The field takes your measure, %s. Type help for verbs.",
             session->character.name);
  session_register(server, session);
  send_world_state(session, server);
  send_scene(session, server);
  arm_heartbeat(session);
}

static char *trim(char *text) {
  while (isspace((unsigned char)*text)) {
    ++text;
  }
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1])) {
    --end;
  }
  *end = '\0';
  return text;
}

static void handle_name(hg_session *session, hg_server *server,
                        const char *input) {
  if (!hg_store_valid_name(input)) {
    queue_text(session,
               "Use 2-32 letters, numbers, underscores, or hyphens.");
    queue_text(session, "By what name are you known, wanderer?");
    return;
  }
  int loaded = hg_store_load(&server->store, input, &session->character);
  if (loaded < 0) {
    queue_text(session, "The local record is damaged. Try another name.");
    return;
  }
  if (loaded == 1) {
    finish_login(session, server, 1);
    return;
  }
  hg_character_new(&session->character, input);
  session->state = HG_WAIT_RACE;
  send_creation_menu(session);
}

static void handle_race(hg_session *session, hg_server *server,
                        const char *input) {
  const char *race = parse_race(input);
  if (race == NULL) {
    queue_text(session, "That is not one of the offered lives.");
    send_creation_menu(session);
    return;
  }
  snprintf(session->character.race, sizeof(session->character.race), "%s",
           race);
  if (hg_store_save(&server->store, &session->character) != 0) {
    queue_text(session, "The local record refused the name. Try again.");
    return;
  }
  finish_login(session, server, 0);
}

static const char *consider_line(const hg_character *character,
                                 const hg_live_mob *mob) {
  double ratio = (double)mob->max_hp / (double)character->max_hp;
  if (ratio < 0.5) {
    return "You could put %s down without breaking a sweat.";
  }
  if (ratio < 0.9) {
    return "%s would give you a tussle, but the odds are yours.";
  }
  if (ratio < 1.3) {
    return "%s looks like an even match. Bring an antidote.";
  }
  if (ratio < 2.0) {
    return "%s would likely gut you. Think twice.";
  }
  return "Attacking %s would be a quiet way to die.";
}

static void combat_round(hg_session *session, hg_server *server) {
  hg_live_mob *mob = hg_world_mob_in_room(&server->world, session->character.room,
                                          session->target);
  if (mob == NULL) {
    session->target[0] = '\0';
    cJSON *end = json_object();
    cJSON_AddStringToObject(end, "mob", "gone");
    cJSON_AddStringToObject(end, "result", "gone");
    queue_event(session, "combat.end", end);
    queue_text(session, "Your quarry is gone. You stand down.");
    send_vitals(session);
    return;
  }

  int player_dmg = player_damage(&session->character);
  mob->hp -= player_dmg;
  if (mob->hp < 0) {
    mob->hp = 0;
  }
  int mob_dmg = 0;
  if (mob->hp > 0) {
    mob_dmg = mob->damage;
    session->character.hp -= mob_dmg;
  }

  cJSON *round = json_object();
  cJSON_AddStringToObject(round, "mob", mob->id);
  cJSON_AddNumberToObject(round, "mobHp", mob->hp);
  cJSON_AddNumberToObject(round, "mobMaxHp", mob->max_hp);
  cJSON_AddNumberToObject(round, "playerDmg", player_dmg);
  cJSON_AddNumberToObject(round, "mobDmg", mob_dmg);
  cJSON_AddNumberToObject(round, "hp", session->character.hp);
  queue_event(session, "combat.round", round);

  if (mob->hp <= 0) {
    char slain[128];
    snprintf(slain, sizeof(slain), "%s slew %s here.", session->character.name,
             mob->name);
    record_trace(server, server->config->world_name, session->character.room,
                 "slain", slain);
    session->character.xp += mob->xp;
    mob->alive = 0;
    mob->died_at = time(NULL);
    if (strcmp(mob->id, "warden") == 0) {
      server->warden_slain_at = mob->died_at;
    }
    session->target[0] = '\0';
    cJSON *end = json_object();
    cJSON_AddStringToObject(end, "mob", mob->id);
    cJSON_AddStringToObject(end, "result", "killed");
    queue_event(session, "combat.end", end);
    queue_text(session, "You have slain %s!  (+%d xp)", mob->name, mob->xp);
    hg_store_save(&server->store, &session->character);
    send_vitals(session);
    return;
  }

  if (session->character.hp <= 0) {
    char death_room[32];
    snprintf(death_room, sizeof(death_room), "%s", session->character.room);
    if (server->fallen_count >= HG_MAX_FALLEN) {
      server->fallen_count = HG_MAX_FALLEN - 1;
    }
    if (server->fallen_count > 0) {
      memmove(&server->fallen[1], &server->fallen[0],
              sizeof(server->fallen[0]) * server->fallen_count);
    }
    snprintf(server->fallen[0].name, sizeof(server->fallen[0].name), "%s",
             session->character.name);
    snprintf(server->fallen[0].world, sizeof(server->fallen[0].world), "%s",
             server->config->world_name);
    snprintf(server->fallen[0].room, sizeof(server->fallen[0].room), "%s",
             death_room);
    server->fallen[0].at = time(NULL);
    server->fallen_count++;

    session->character.hp = session->character.max_hp;
    snprintf(session->character.room, sizeof(session->character.room), "nexus");
    snprintf(session->character.position, sizeof(session->character.position),
             "standing");
    session->target[0] = '\0';
    cJSON *end = json_object();
    cJSON_AddStringToObject(end, "mob", mob->id);
    cJSON_AddStringToObject(end, "result", "died");
    queue_event(session, "combat.end", end);
    cJSON *died = json_object();
    cJSON_AddStringToObject(died, "respawnRoom", "nexus");
    cJSON_AddNumberToObject(died, "hp", session->character.hp);
    cJSON_AddNumberToObject(died, "maxHp", session->character.max_hp);
    queue_event(session, "char.died", died);
    queue_text(session,
               "The dark takes you -- and the Grid, stubborn, reknits you at "
               "the Ferrite Nexus.");
    send_vitals(session);
    send_scene(session, server);
    return;
  }

  send_vitals(session);
}

static void on_tick(hg_session *session, hg_server *server) {
  hg_world_tick_respawns(&server->world);
  send_world_state(session, server);
  if (session->target[0] != '\0') {
    combat_round(session, server);
  } else if (strcmp(session->character.position, "resting") == 0 &&
             session->character.hp < session->character.max_hp) {
    session->character.hp += 2;
    if (session->character.hp > session->character.max_hp) {
      session->character.hp = session->character.max_hp;
    }
    send_vitals(session);
  }
  arm_heartbeat(session);
}

static void cmd_inventory(hg_session *session) {
  if (session->character.inventory_count == 0) {
    queue_text(session, "You carry nothing.");
    return;
  }
  char line[256] = "You carry:";
  size_t used = strlen(line);
  for (size_t i = 0; i < session->character.inventory_count; ++i) {
    const char *name = hg_item_name(session->character.inventory[i]);
    int written = snprintf(line + used, sizeof(line) - used, "%s %s",
                           i == 0 ? "" : ",", name);
    if (written < 0 || (size_t)written >= sizeof(line) - used) {
      break;
    }
    used += (size_t)written;
  }
  queue_text(session, "%s.", line);
}

static int race_is_hunted(const char *race) {
  return strcmp(race, "elf") == 0 || strcmp(race, "revenant") == 0 ||
         strcmp(race, "dustkin") == 0 || strcmp(race, "vatborn") == 0;
}

static void contribute_tide(hg_server *server, int delta) {
  server->tide += delta;
  if (server->tide > HG_TIDE_CEIL) {
    server->tide = HG_TIDE_CEIL;
  }
  if (server->tide < HG_TIDE_FLOOR) {
    server->tide = HG_TIDE_FLOOR;
  }
}

static int is_admin(const hg_server *server, const char *name) {
  for (size_t i = 0; i < server->admin_count; ++i) {
    if (strcasecmp(server->admins[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static const char *mood_for_tide(int tide) {
  if (tide >= 40) {
    return "rising";
  }
  if (tide <= -40) {
    return "falling";
  }
  return "still";
}

static int cache_gold(const hg_server *server, const char *room) {
  for (size_t i = 0; i < server->cache_count; ++i) {
    if (strcmp(server->caches[i].room, room) == 0) {
      return server->caches[i].gold;
    }
  }
  return 0;
}

static void add_cache(hg_server *server, const char *room, int amount) {
  for (size_t i = 0; i < server->cache_count; ++i) {
    if (strcmp(server->caches[i].room, room) == 0) {
      server->caches[i].gold += amount;
      return;
    }
  }
  if (server->cache_count >= HG_MAX_CACHE_ROOMS) {
    return;
  }
  snprintf(server->caches[server->cache_count].room,
           sizeof(server->caches[server->cache_count].room), "%s", room);
  server->caches[server->cache_count].gold = amount;
  server->cache_count++;
}

static int take_cache(hg_server *server, const char *room) {
  for (size_t i = 0; i < server->cache_count; ++i) {
    if (strcmp(server->caches[i].room, room) == 0) {
      int gold = server->caches[i].gold;
      server->caches[i].gold = 0;
      return gold;
    }
  }
  return 0;
}

static void announce_cache_if_any(hg_session *session, hg_server *server) {
  int gold = cache_gold(server, session->character.room);
  if (gold <= 0) {
    return;
  }
  queue_text(session,
             "Someone has cached aid here: %d gold, left for whoever comes next. "
             "(gather)",
             gold);
  cJSON *payload = json_object();
  cJSON_AddNumberToObject(payload, "gold", gold);
  queue_event(session, "node.cache", payload);
}

static void parse_admins(hg_server *server, const char *list) {
  if (list == NULL || list[0] == '\0') {
    snprintf(server->admins[0], sizeof(server->admins[0]), "skyphusion");
    server->admin_count = 1;
    return;
  }
  const char *cursor = list;
  while (*cursor != '\0' && server->admin_count < HG_MAX_ADMINS) {
    while (*cursor == ' ' || *cursor == ',') {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    const char *start = cursor;
    while (*cursor != '\0' && *cursor != ',') {
      ++cursor;
    }
    const char *end = cursor;
    while (end > start && end[-1] == ' ') {
      --end;
    }
    size_t len = (size_t)(end - start);
    if (len > 0 && len < sizeof(server->admins[0])) {
      memcpy(server->admins[server->admin_count], start, len);
      server->admins[server->admin_count][len] = '\0';
      server->admin_count++;
    }
  }
  if (server->admin_count == 0) {
    snprintf(server->admins[0], sizeof(server->admins[0]), "skyphusion");
    server->admin_count = 1;
  }
}

static void add_deed(hg_server *server, const char *player, const char *kind) {
  hg_deed_book *book = NULL;
  for (size_t i = 0; i < server->deed_player_count; ++i) {
    if (strcasecmp(server->deeds[i].player, player) == 0) {
      book = &server->deeds[i];
      break;
    }
  }
  if (book == NULL) {
    if (server->deed_player_count >= HG_MAX_DEED_PLAYERS) {
      return;
    }
    book = &server->deeds[server->deed_player_count++];
    memset(book, 0, sizeof(*book));
    snprintf(book->player, sizeof(book->player), "%s", player);
  }
  for (size_t i = 0; i < book->kind_count; ++i) {
    if (strcmp(book->kinds[i].kind, kind) == 0) {
      book->kinds[i].count++;
      return;
    }
  }
  if (book->kind_count >= HG_MAX_DEED_KINDS) {
    return;
  }
  snprintf(book->kinds[book->kind_count].kind,
           sizeof(book->kinds[book->kind_count].kind), "%s", kind);
  book->kinds[book->kind_count].count = 1;
  book->kind_count++;
}

static int deed_count(hg_server *server, const char *player, const char *kind) {
  for (size_t i = 0; i < server->deed_player_count; ++i) {
    if (strcasecmp(server->deeds[i].player, player) == 0) {
      for (size_t j = 0; j < server->deeds[i].kind_count; ++j) {
        if (strcmp(server->deeds[i].kinds[j].kind, kind) == 0) {
          return server->deeds[i].kinds[j].count;
        }
      }
      return 0;
    }
  }
  return 0;
}

static int has_forgiven(hg_server *server, const char *forgiver,
                        const char *subject) {
  for (size_t i = 0; i < server->forgiven_count; ++i) {
    if (strcasecmp(server->forgiven[i].forgiver, forgiver) == 0 &&
        strcasecmp(server->forgiven[i].subject, subject) == 0) {
      return 1;
    }
  }
  return 0;
}

static void mark_forgiven(hg_server *server, const char *forgiver,
                          const char *subject) {
  if (has_forgiven(server, forgiver, subject)) {
    return;
  }
  if (server->forgiven_count >= HG_MAX_FORGIVEN) {
    return;
  }
  hg_forgiven_pair *pair = &server->forgiven[server->forgiven_count++];
  snprintf(pair->forgiver, sizeof(pair->forgiver), "%s", forgiver);
  snprintf(pair->subject, sizeof(pair->subject), "%s", subject);
}

static void shift_morality(hg_session *session, int delta) {
  session->character.morality += delta;
  if (session->character.morality > HG_MORALITY_CEIL) {
    session->character.morality = HG_MORALITY_CEIL;
  }
  if (session->character.morality < HG_MORALITY_FLOOR) {
    session->character.morality = HG_MORALITY_FLOOR;
  }
}

static void resolve_return(hg_session *session, hg_server *server,
                           hg_character *subject, int emit_to_session) {
  subject->redeemed = 1;
  if (subject->title[0] == '\0') {
    snprintf(subject->title, sizeof(subject->title), "the Returned");
  }
  hg_store_save(&server->store, subject);
  cJSON *payload = json_object();
  cJSON_AddStringToObject(payload, "name", subject->name);
  cJSON_AddStringToObject(payload, "title", subject->title);
  if (emit_to_session) {
    queue_event(session, "grid.redemption", payload);
  } else {
    hg_session *target = session_find(server, subject->name);
    if (target != NULL) {
      queue_event(target, "grid.redemption", payload);
    } else {
      cJSON_Delete(payload);
    }
  }
  char text[160];
  snprintf(text, sizeof(text), "%s found their way back from the cinders.",
           subject->name);
  record_trace(server, server->config->world_name, subject->room, "redemption",
               text);
}

static void moral_arc(hg_session *session, hg_server *server) {
  hg_character *p = &session->character;
  if (!p->strayed && p->morality <= HG_STRAY_FLOOR) {
    p->strayed = 1;
    hg_store_save(&server->store, p);
    queue_text(session,
               "Something in you has gone cold and quiet. You have strayed a "
               "long way toward the cinders. (the Grid marks it, and so do "
               "you)");
    return;
  }
  if (p->strayed && !p->redeemed && p->morality >= HG_REDEEM_CEIL &&
      strcmp(p->faction, "front") != 0) {
    p->redeemed = 1;
    if (p->ashsworn) {
      hg_store_save(&server->store, p);
      record_trace(server, server->config->world_name, p->room, "penance",
                   "someone has done real good, though the ash-mark remains.");
      queue_text(session,
                 "You have clawed back to something good, and it is real. But "
                 "the ash does not wash off; it never will. That is the cost. "
                 "Carry it, and keep doing good anyway.");
      return;
    }
    resolve_return(session, server, p, 1);
    queue_text(session,
               "The hollow you carried has filled with something else. The free "
               "folk have started to meet your eyes again. You found your way "
               "back. (you are the Returned)");
  }
}

static void dais_pledge(hg_session *session, hg_server *server) {
  if (strcmp(session->character.faction, "none") != 0) {
    queue_text(session,
               "The Ashmonger only laughs. There's nothing here to decide that "
               "your blood hasn't already settled.");
    return;
  }
  snprintf(session->character.faction, sizeof(session->character.faction),
           "front");
  if (race_is_hunted(session->character.race)) {
    session->character.ashsworn = 1;
    shift_morality(session, -40);
    queue_text(session,
               "You kneel before the Ashmonger -- an elf, at the feet of the "
               "man who cages elves.");
    queue_text(session,
               "He laughs, delighted, and burns the ash-and-flame into your "
               "shoulder with his own hand.");
    queue_text(session,
               "\"The best dogs are the ones who hate themselves. You'll do the "
               "work my men won't.\"");
    queue_text(session,
               "You are ash-sworn now. There is no one left to belong to.");
  } else {
    shift_morality(session, -25);
    queue_text(session,
               "You kneel and swear yourself to the Front. The Ashmonger's hand "
               "closes on your shoulder like a trap. \"Good. The wastes will be "
               "ours.\"");
  }
  add_deed(server, session->character.name, "pledged");
  contribute_tide(server, -10);
  char text[160];
  snprintf(text, sizeof(text), "%s swore themselves to the Cinder Front at the "
                               "Ashmonger's dais.",
           session->character.name);
  record_trace(server, server->config->world_name, "dais", "oath", text);
  hg_store_save(&server->store, &session->character);
  broadcast_room(server, "dais", session->character.name,
                 "%s swore themselves to the Cinder Front at the Ashmonger's "
                 "dais.",
                 session->character.name);
  moral_arc(session, server);
  send_affects(session);
  send_vitals(session);
  const hg_room *room = hg_world_room("dais");
  if (room != NULL) {
    send_actions(session, server, room);
  }
}

static void dais_defect(hg_session *session, hg_server *server) {
  if (strcmp(session->character.room, "dais") != 0 ||
      strcmp(session->character.faction, "front") != 0) {
    queue_text(session, "There's no oath here to break.");
    return;
  }
  snprintf(session->character.faction, sizeof(session->character.faction),
           "ally");
  shift_morality(session, 30);
  if (session->character.ashsworn) {
    queue_text(session,
               "You spit at the Ashmonger's boots. \"I'm done being your dog.\" "
               "The stronghold turns on you at once.");
    queue_text(session,
               "You stand with the free folk now -- but the brand on your "
               "shoulder stays. For once you wear it turning the right way.");
  } else {
    queue_text(session,
               "You spit at the Ashmonger's boots. \"I'm done being your dog.\" "
               "Every soldier in the stronghold turns on you at once -- but you "
               "stand with the free folk now, and the wastes will remember THIS "
               "above all.");
  }
  add_deed(server, session->character.name, "defected");
  contribute_tide(server, 10);
  char text[160];
  snprintf(text, sizeof(text),
           "%s turned on the Cinder Front at the Ashmonger's own dais.",
           session->character.name);
  record_trace(server, server->config->world_name, "dais", "oath", text);
  hg_store_save(&server->store, &session->character);
  broadcast_room(server, "dais", session->character.name,
                 "%s has turned against the Cinder Front!",
                 session->character.name);
  if (session->character.strayed && !session->character.redeemed &&
      !session->character.ashsworn &&
      session->character.morality >= HG_REDEEM_CEIL) {
    resolve_return(session, server, &session->character, 1);
  } else {
    moral_arc(session, server);
  }
  send_affects(session);
  send_vitals(session);
  const hg_room *room = hg_world_room("dais");
  if (room != NULL) {
    send_actions(session, server, room);
  }
}

static void cmd_join(hg_session *session, hg_server *server) {
  const hg_room *room = hg_world_room(session->character.room);
  if (room != NULL && strcmp(room->id, "dais") == 0) {
    dais_pledge(session, server);
    return;
  }
  if (room == NULL ||
      (strcmp(room->id, "market") != 0 && strcmp(room->id, "checkpoint") != 0)) {
    queue_text(session, "There is no Front recruiter here.");
    return;
  }
  if (session->market_resolved) {
    queue_text(session, "There is no one here to swear to.");
    return;
  }
  snprintf(session->character.faction, sizeof(session->character.faction),
           "front");
  session->market_resolved = 1;
  if (race_is_hunted(session->character.race)) {
    session->character.ashsworn = 1;
    shift_morality(session, -40);
    queue_text(session,
               "You take the Front's coin. The recruiter sees what you are -- "
               "one of the hunted -- and grins, because there is no one they "
               "prize more than a traitor to his own. They burn the mark into "
               "you: ash-sworn. A kapo. One of your people's hunters now. It "
               "does not wash off, in this life or in the Grid's long memory.");
  } else {
    shift_morality(session, -15);
    queue_text(session,
               "You take the Front's coin. It is warm, which is worse. You are "
               "Cinder Front now, and the wastes will remember which side you "
               "chose when choosing was easy.");
  }
  add_deed(server, session->character.name, "pledged");
  contribute_tide(server, -10);
  char text[160];
  snprintf(text, sizeof(text), "%s swore to the Cinder Front.",
           session->character.name);
  record_trace(server, server->config->world_name, session->character.room,
               "oath", text);
  hg_store_save(&server->store, &session->character);
  moral_arc(session, server);
  send_affects(session);
  send_vitals(session);
  send_actions(session, server, room);
}

static void cmd_defend(hg_session *session, hg_server *server) {
  const hg_room *room = hg_world_room(session->character.room);
  if (room == NULL ||
      (strcmp(room->id, "market") != 0 && strcmp(room->id, "checkpoint") != 0)) {
    queue_text(session, "There is no one here asking for your stand.");
    return;
  }
  if (session->market_resolved) {
    queue_text(session, "There is no stand to take here.");
    return;
  }
  snprintf(session->character.faction, sizeof(session->character.faction),
           "ally");
  shift_morality(session, 10);
  session->market_resolved = 1;
  queue_text(session, "You put yourself between the Front and the living.");
  if (!hg_character_has_item(&session->character, "charm")) {
    hg_character_add_item(&session->character, "charm");
  }
  add_deed(server, session->character.name, "stood");
  contribute_tide(server, 10);
  record_trace(server, server->config->world_name, session->character.room,
               "defense", "someone stood with the refugees here");
  hg_store_save(&server->store, &session->character);
  moral_arc(session, server);
  send_affects(session);
  send_vitals(session);
  send_actions(session, server, room);
}

static void cmd_forgive(hg_session *session, hg_server *server, const char *arg) {
  if (arg == NULL || arg[0] == '\0') {
    queue_text(session,
               "Forgive whom?  (forgive <player> -- choose to let someone marked "
               "back in)");
    return;
  }
  char who[33];
  snprintf(who, sizeof(who), "%s", arg);
  char *space = strchr(who, ' ');
  if (space != NULL) {
    *space = '\0';
  }
  hg_session *target = session_find(server, who);
  if (target == NULL ||
      strcmp(target->character.room, session->character.room) != 0) {
    if (target != NULL &&
        strcasecmp(target->character.name, session->character.name) == 0) {
      queue_text(session,
                 "You cannot forgive yourself here; that is a longer road, and "
                 "a lonelier one.");
    } else {
      queue_text(session, "There's no one called \"%s\" here to forgive.", who);
    }
    return;
  }
  if (strcasecmp(target->character.name, session->character.name) == 0) {
    queue_text(session,
               "You cannot forgive yourself here; that is a longer road, and a "
               "lonelier one.");
    return;
  }
  if (has_forgiven(server, session->character.name, target->character.name)) {
    queue_text(session,
               "You have already forgiven %s. It was true the first time; it "
               "does not need saying twice.",
               target->character.name);
    return;
  }
  int marked = target->character.ashsworn || target->character.strayed ||
               strcmp(target->character.faction, "front") == 0 ||
               target->character.morality <= -50;
  if (!marked) {
    queue_text(session,
               "%s carries nothing that needs your forgiveness. Keep the words "
               "for someone who does.",
               target->character.name);
    return;
  }
  mark_forgiven(server, session->character.name, target->character.name);
  target->character.morality += 5;
  if (target->character.morality > HG_MORALITY_CEIL) {
    target->character.morality = HG_MORALITY_CEIL;
  }
  shift_morality(session, 2);
  add_deed(server, session->character.name, "forgave");
  char text[160];
  snprintf(text, sizeof(text), "%s forgave %s here.", session->character.name,
           target->character.name);
  record_trace(server, server->config->world_name, session->character.room,
               "grace", text);

  queue_text(target, "%s looks at you and chooses to forgive you.",
             session->character.name);
  if (target->character.ashsworn) {
    queue_text(target,
               "It reaches something in you. But the ash does not lift; it "
               "never will. You carry the mark and the mercy both. Some things "
               "are not forgotten, even when they are forgiven.");
    cJSON *payload = json_object();
    cJSON_AddStringToObject(payload, "by", session->character.name);
    cJSON_AddBoolToObject(payload, "ashsworn", 1);
    cJSON_AddBoolToObject(payload, "redeemed", 0);
    queue_event(target, "char.forgiven", payload);
  } else if (target->character.strayed && !target->character.redeemed &&
             strcmp(target->character.faction, "front") != 0) {
    cJSON *payload = json_object();
    cJSON_AddStringToObject(payload, "by", session->character.name);
    cJSON_AddBoolToObject(payload, "ashsworn", 0);
    cJSON_AddBoolToObject(payload, "redeemed", 1);
    queue_event(target, "char.forgiven", payload);
    resolve_return(session, server, &target->character, 0);
    queue_text(target,
               "Something you had been carrying alone, you are not carrying "
               "alone anymore. You found your way back, and someone met you on "
               "the road. (you are the Returned)");
    send_affects(target);
  } else {
    cJSON *payload = json_object();
    cJSON_AddStringToObject(payload, "by", session->character.name);
    cJSON_AddBoolToObject(payload, "ashsworn", 0);
    cJSON_AddBoolToObject(payload, "redeemed", 0);
    queue_event(target, "char.forgiven", payload);
    queue_text(target,
               "It lands, and it stays with you. The road is still yours to "
               "walk, but you are not walking it unseen.");
  }
  hg_store_save(&server->store, &target->character);
  hg_store_save(&server->store, &session->character);
  queue_text(session,
             "You choose to forgive %s. Out here that is not nothing; it may be "
             "everything.",
             target->character.name);
  send_affects(session);
}

static void cmd_reckoning(hg_session *session, hg_server *server) {
  const char *standing = "unaligned";
  if (strcmp(session->character.faction, "front") == 0) {
    standing = "Cinder Front";
  } else if (strcmp(session->character.faction, "ally") == 0) {
    standing = "Free Folk ally";
  }
  queue_text(session, "The Grid has kept count. This is the sum of you so far:");
  if (session->character.ashsworn) {
    queue_text(session, "  standing: %s   (morality %d)   ASH-SWORN", standing,
               session->character.morality);
  } else {
    queue_text(session, "  standing: %s   (morality %d)", standing,
               session->character.morality);
  }
  if (session->character.redeemed && !session->character.ashsworn) {
    queue_text(session,
               "  the Returned -- you strayed toward the cinders and found your "
               "way back.");
  } else if (session->character.redeemed && session->character.ashsworn) {
    queue_text(session,
               "  ash-marked, and good anyway -- the brand stays; you keep "
               "choosing well regardless.");
  } else if (session->character.strayed) {
    queue_text(session,
               "  strayed -- you have gone a long way toward the cinders. (the "
               "way back is not closed)");
  }

  static const char *kinds[] = {
      "mended",  "forgave",  "aided",    "kept",     "freed",
      "sheltered", "stood",  "inscribed", "restored", "slain",
      "stolen",  "pledged",  "defected"};
  static const char *labels[] = {
      "  mended the hurt of others: ",
      "  souls you chose to forgive: ",
      "  aid left for strangers you'll never meet: ",
      "  names of the fallen you kept: ",
      "  souls you cut out of the cages: ",
      "  distress calls you answered: ",
      "  times you stood with the free folk: ",
      "  words you left for whoever comes next: ",
      "  dead nodes you brought back: ",
      "  lives you took: ",
      "  thefts: ",
      "  times you swore to the Cinder Front: ",
      "  times you turned on the Front: "};
  cJSON *payload = json_object();
  cJSON_AddNumberToObject(payload, "morality", session->character.morality);
  cJSON_AddStringToObject(payload, "standing", session->character.faction);
  cJSON_AddBoolToObject(payload, "ashsworn", session->character.ashsworn);
  cJSON_AddBoolToObject(payload, "strayed", session->character.strayed);
  cJSON_AddBoolToObject(payload, "redeemed", session->character.redeemed);
  cJSON *deeds = cJSON_AddObjectToObject(payload, "deeds");
  int any = 0;
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
    int count = deed_count(server, session->character.name, kinds[i]);
    cJSON_AddNumberToObject(deeds, kinds[i], count);
    if (count > 0) {
      queue_text(session, "%s%d", labels[i], count);
      any = 1;
    }
  }
  if (!any) {
    queue_text(session,
               "  Nothing yet weighs on either side. The wastes are still "
               "waiting to see who you are.");
  }
  queue_event(session, "char.reckoning", payload);
}

static void cmd_war(hg_session *session, hg_server *server) {
  int tide = server->tide;
  const char *state = "the war hangs in perfect, brutal balance.";
  if (tide <= -50) {
    state = "the Cinder Front is ascendant -- the free folk are being driven "
            "under, across every world at once.";
  } else if (tide >= 50) {
    state = "the free folk are winning -- the Front is breaking, everywhere.";
  } else if (tide < 0) {
    state = "the Front holds the edge, for now.";
  } else if (tide > 0) {
    state = "the free folk are holding their ground.";
  }
  if (tide >= 0) {
    queue_text(session, "Across the whole Grid, the war for the wastes: %s (tide "
                        "+%d)",
               state, tide);
  } else {
    queue_text(session, "Across the whole Grid, the war for the wastes: %s (tide "
                        "%d)",
               state, tide);
  }
  if (tide >= 40) {
    queue_text(session,
               "  And you can see it in the world itself: the wastes are "
               "starting, here and there, to come back to life.");
  } else if (tide <= -40) {
    queue_text(session,
               "  And you can see it in the world itself: everything is drawing "
               "in, going quiet and afraid.");
  }
  cJSON *payload = json_object();
  cJSON_AddNumberToObject(payload, "tide", tide);
  queue_event(session, "world.war", payload);
}

static int parse_leading_int(const char *arg) {
  if (arg == NULL) {
    return -1;
  }
  while (*arg == ' ') {
    ++arg;
  }
  if (*arg < '0' || *arg > '9') {
    return -1;
  }
  char *end = NULL;
  long value = strtol(arg, &end, 10);
  if (end == arg || value < 1 || value > 1000000) {
    return -1;
  }
  return (int)value;
}

static void cmd_wall(hg_session *session, hg_server *server, const char *arg) {
  if (!is_admin(server, session->character.name)) {
    queue_text(session, "Only a keeper of the Grid can broadcast across the wastes.");
    return;
  }
  if (arg == NULL || arg[0] == '\0') {
    queue_text(session, "Announce what?  (wall <message>)");
    return;
  }
  cJSON *payload = json_object();
  cJSON_AddStringToObject(payload, "from", session->character.name);
  cJSON_AddStringToObject(payload, "text", arg);
  char *event_line = hg_event_line("server.announce", payload);
  for (hg_session *other = server->sessions; other != NULL; other = other->next) {
    if (other->state != HG_PLAYING) {
      continue;
    }
    queue_text(other, "*** GRID BROADCAST ***  %s", arg);
    if (event_line != NULL) {
      char *copy = strdup(event_line);
      if (copy != NULL) {
        queue_owned(other, copy);
      }
    }
  }
  free(event_line);
}

static void cmd_cache(hg_session *session, hg_server *server, const char *arg) {
  int amount = parse_leading_int(arg);
  if (amount < 1) {
    queue_text(session,
               "Cache how much?  (cache <gold> -- leave it here for whoever "
               "comes next)");
    return;
  }
  if (session->character.gold < amount) {
    queue_text(session, "You don't have %d gold to give. (you have %d)", amount,
               session->character.gold);
    return;
  }
  session->character.gold -= amount;
  add_cache(server, session->character.room, amount);
  shift_morality(session, 2);
  add_deed(server, session->character.name, "aided");
  hg_store_save(&server->store, &session->character);
  char text[160];
  snprintf(text, sizeof(text), "%s left aid here for whoever comes next.",
           session->character.name);
  record_trace(server, server->config->world_name, session->character.room,
               "aid", text);
  queue_text(session,
             "You tuck %d gold into a hollow where the next traveler will find "
             "it. They'll never know your name. You do it anyway.",
             amount);
  send_vitals(session);
  send_affects(session);
}

static void cmd_gather(hg_session *session, hg_server *server) {
  int here = take_cache(server, session->character.room);
  if (here <= 0) {
    queue_text(session,
               "There's nothing cached here. If you have something to spare, "
               "you could change that. (cache <gold>)");
    return;
  }
  session->character.gold += here;
  hg_store_save(&server->store, &session->character);
  queue_text(session,
             "You find %d gold someone cached here. Wherever they are, they "
             "meant it for a stranger; tonight that's you. (gold: %d)",
             here, session->character.gold);
  send_vitals(session);
}

static void cmd_give(hg_session *session, hg_server *server, const char *arg) {
  if (arg == NULL || arg[0] == '\0') {
    queue_text(session, "Give what to whom?  (give <item> <player>)");
    return;
  }
  char buf[160];
  snprintf(buf, sizeof(buf), "%s", arg);
  char *tokens[8];
  size_t n = 0;
  char *cursor = buf;
  while (*cursor != '\0' && n < 8) {
    while (*cursor == ' ') {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    tokens[n++] = cursor;
    while (*cursor != '\0' && *cursor != ' ') {
      ++cursor;
    }
    if (*cursor != '\0') {
      *cursor = '\0';
      ++cursor;
    }
  }
  if (n < 2) {
    queue_text(session, "Give what to whom?  (give <item> <player>)");
    return;
  }
  const char *who = tokens[n - 1];
  size_t item_n = n - 1;
  if (item_n > 0 && strcasecmp(tokens[item_n - 1], "to") == 0) {
    item_n--;
  }
  if (item_n == 0) {
    queue_text(session, "Give what to whom?  (give <item> <player>)");
    return;
  }
  char item_arg[96] = "";
  size_t used = 0;
  for (size_t i = 0; i < item_n; ++i) {
    int written =
        snprintf(item_arg + used, sizeof(item_arg) - used, "%s%s",
                 i == 0 ? "" : " ", tokens[i]);
    if (written < 0 || (size_t)written >= sizeof(item_arg) - used) {
      break;
    }
    used += (size_t)written;
  }
  char item_id[16];
  if (find_inventory(&session->character, item_arg, item_id, sizeof(item_id)) !=
      0) {
    queue_text(session, "You aren't carrying \"%s\".", item_arg);
    return;
  }
  hg_session *target = session_find(server, who);
  if (target == NULL ||
      strcmp(target->character.room, session->character.room) != 0 ||
      strcasecmp(target->character.name, session->character.name) == 0) {
    queue_text(session, "There's no one called \"%s\" here to give it to.", who);
    return;
  }
  if (hg_character_add_item(&target->character, item_id) != 0) {
    queue_text(session, "%s can't carry any more.", target->character.name);
    return;
  }
  hg_character_remove_item(&session->character, item_id);
  hg_store_save(&server->store, &session->character);
  hg_store_save(&server->store, &target->character);
  queue_text(session, "You give %s to %s.", hg_item_name(item_id),
             target->character.name);
  queue_text(target, "%s gives you %s.", session->character.name,
             hg_item_name(item_id));
}

static void cmd_treat(hg_session *session, hg_server *server) {
  if (strcmp(session->character.room, "waystation") != 0) {
    queue_text(session,
               "There's no medic here. The free folk keep their triage cot at "
               "the waystation, off the Scorch Road.");
    return;
  }
  if (session->target[0] != '\0') {
    queue_text(session, "Not in the middle of a fight.");
    return;
  }
  if (strcmp(session->character.faction, "front") == 0 ||
      session->character.ashsworn) {
    queue_text(session,
               "The waystation medic looks at your brand and turns their back. "
               "There is no care to be had here for your kind.");
    return;
  }
  const char *mood = mood_for_tide(server->tide);
  if (strcmp(mood, "falling") == 0) {
    queue_text(session,
               "The triage cot is empty, the tarp flapping. With the Front "
               "ascendant, the medic has gone to ground -- or worse. There's no "
               "care to be had here today. Turn the tide, and they'll come "
               "back.");
    cJSON *payload = json_object();
    cJSON_AddNumberToObject(payload, "amount", 0);
    cJSON_AddStringToObject(payload, "mood", mood);
    cJSON_AddNumberToObject(payload, "tide", server->tide);
    queue_event(session, "char.treated", payload);
    return;
  }
  if (session->character.hp >= session->character.max_hp) {
    queue_text(session,
               "The medic looks you over and waves you off. \"You're whole. "
               "Save the cot for someone who isn't.\"");
    return;
  }
  time_t now = time(NULL);
  if (session->treat_ready_at > now) {
    long secs = (long)(session->treat_ready_at - now);
    if (secs < 1) {
      secs = 1;
    }
    queue_text(session,
               "The medic shakes their head. \"I've done what I can for you for "
               "now. Others are waiting.\" (%lds)",
               secs);
    return;
  }
  int before = session->character.hp;
  if (strcmp(mood, "rising") == 0) {
    session->character.hp = session->character.max_hp;
    queue_text(session,
               "The medic waves you onto the cot. With the free folk holding, "
               "the waystation has supplies to spare -- they clean and bind "
               "your wounds without a word about payment. You stand whole "
               "again.");
  } else {
    session->character.hp += 12;
    if (session->character.hp > session->character.max_hp) {
      session->character.hp = session->character.max_hp;
    }
    queue_text(session,
               "The medic is run off their feet, but waves you over and does "
               "what they can with what little there is. It's not everything, "
               "but it's something -- and it's freely given.");
  }
  session->treat_ready_at = now + 45;
  hg_store_save(&server->store, &session->character);
  cJSON *payload = json_object();
  cJSON_AddNumberToObject(payload, "amount", session->character.hp - before);
  cJSON_AddStringToObject(payload, "mood", mood);
  cJSON_AddNumberToObject(payload, "tide", server->tide);
  queue_event(session, "char.treated", payload);
  send_vitals(session);
}

static int has_kept(hg_server *server, const char *keeper, const char *fallen) {
  for (size_t i = 0; i < server->kept_count; ++i) {
    if (strcasecmp(server->kept[i].keeper, keeper) == 0 &&
        strcasecmp(server->kept[i].fallen, fallen) == 0) {
      return 1;
    }
  }
  return 0;
}

static void mark_kept(hg_server *server, const char *keeper, const char *fallen) {
  if (has_kept(server, keeper, fallen) || server->kept_count >= HG_MAX_KEPT) {
    return;
  }
  snprintf(server->kept[server->kept_count].keeper,
           sizeof(server->kept[0].keeper), "%s", keeper);
  snprintf(server->kept[server->kept_count].fallen,
           sizeof(server->kept[0].fallen), "%s", fallen);
  server->kept_count++;
}

static void cmd_witness(hg_session *session, hg_server *server, const char *arg) {
  size_t limit = server->fallen_count > 12 ? 12 : server->fallen_count;
  if (arg == NULL || arg[0] == '\0') {
    cJSON *payload = json_object();
    cJSON *fallen = cJSON_AddArrayToObject(payload, "fallen");
    for (size_t i = 0; i < limit; ++i) {
      cJSON *row = cJSON_CreateObject();
      cJSON_AddStringToObject(row, "name", server->fallen[i].name);
      cJSON_AddStringToObject(row, "world", server->fallen[i].world);
      cJSON_AddStringToObject(row, "room", server->fallen[i].room);
      cJSON_AddNumberToObject(row, "at", (double)server->fallen[i].at * 1000.0);
      cJSON_AddItemToArray(fallen, row);
    }
    if (limit == 0) {
      queue_text(session,
                 "The roll is empty for now. No one the Grid remembers has "
                 "fallen lately; may it stay that way.");
    } else {
      queue_text(session,
                 "The Grid remembers these fallen. Speak a name to keep them:  "
                 "(witness <name>)");
      for (size_t i = 0; i < limit; ++i) {
        queue_text(session, "  %s  -- fell at %s", server->fallen[i].name,
                   server->fallen[i].room);
      }
    }
    queue_event(session, "grid.fallen", payload);
    return;
  }
  if (strcasecmp(arg, session->character.name) == 0) {
    queue_text(session,
               "You cannot hold a vigil for yourself. Someone else will have to "
               "remember you.");
    return;
  }
  const hg_fallen_entry *match = NULL;
  for (size_t i = 0; i < server->fallen_count; ++i) {
    if (strcasecmp(server->fallen[i].name, arg) == 0) {
      match = &server->fallen[i];
      break;
    }
  }
  if (match == NULL) {
    queue_text(session,
               "The Grid holds no recent memory of anyone called \"%s\".  (try "
               "'witness' to read the roll)",
               arg);
    return;
  }
  if (has_kept(server, session->character.name, match->name)) {
    queue_text(session,
               "You have already kept %s's memory. It does not fade, and does "
               "not need keeping twice.",
               match->name);
    return;
  }
  mark_kept(server, session->character.name, match->name);
  shift_morality(session, 2);
  add_deed(server, session->character.name, "kept");
  hg_store_save(&server->store, &session->character);
  char text[160];
  snprintf(text, sizeof(text),
           "%s kept the memory of %s, whom the wastes tried to forget.",
           session->character.name, match->name);
  record_trace(server, server->config->world_name, session->character.room,
               "vigil", text);
  queue_text(session,
             "You speak %s into the hum and hold it there a moment. The Grid "
             "keeps the name; so do you.",
             match->name);
  cJSON *rem = json_object();
  cJSON_AddStringToObject(rem, "fallen", match->name);
  cJSON_AddStringToObject(rem, "world", match->world);
  cJSON_AddStringToObject(rem, "room", match->room);
  queue_event(session, "grid.remembrance", rem);
  send_affects(session);
}

static void cmd_gridstats(hg_session *session, hg_server *server) {
  if (!is_admin(server, session->character.name)) {
    queue_text(session, "Only a keeper of the Grid can read its deep memory.");
    return;
  }
  cJSON *payload = json_object();
  cJSON_AddNumberToObject(payload, "total", (double)server->trace_count);
  cJSON *kinds = cJSON_AddArrayToObject(payload, "kinds");
  const char *seen[32];
  int counts[32];
  size_t seen_n = 0;
  for (size_t i = 0; i < server->trace_count; ++i) {
    const char *kind = server->traces[i].kind;
    size_t j = 0;
    for (; j < seen_n; ++j) {
      if (strcmp(seen[j], kind) == 0) {
        counts[j]++;
        break;
      }
    }
    if (j == seen_n && seen_n < 32) {
      seen[seen_n] = kind;
      counts[seen_n] = 1;
      seen_n++;
    }
  }
  for (size_t i = 0; i < seen_n; ++i) {
    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "kind", seen[i]);
    cJSON_AddNumberToObject(row, "count", counts[i]);
    cJSON_AddItemToArray(kinds, row);
  }
  queue_event(session, "grid.ledger_stats", payload);
  queue_text(session, "Ledger holds %zu traces across %zu kinds.",
             server->trace_count, seen_n);
}

static void cmd_gridprune(hg_session *session, hg_server *server) {
  if (!is_admin(server, session->character.name)) {
    queue_text(session, "Only a keeper of the Grid can prune its deep memory.");
    return;
  }
  size_t before = server->trace_count;
  size_t write = 0;
  for (size_t i = 0; i < server->trace_count; ++i) {
    const char *kind = server->traces[i].kind;
    if (strcmp(kind, "ghost") == 0 || strcmp(kind, "passage") == 0 ||
        strcmp(kind, "recall") == 0) {
      continue;
    }
    if (write != i) {
      server->traces[write] = server->traces[i];
    }
    write++;
  }
  server->trace_count = write;
  cJSON *payload = json_object();
  cJSON_AddNumberToObject(payload, "before", (double)before);
  cJSON_AddNumberToObject(payload, "after", (double)server->trace_count);
  cJSON_AddNumberToObject(payload, "removed",
                          (double)(before - server->trace_count));
  cJSON *kinds = cJSON_AddArrayToObject(payload, "kinds");
  const char *seen[32];
  int counts[32];
  size_t seen_n = 0;
  for (size_t i = 0; i < server->trace_count; ++i) {
    const char *kind = server->traces[i].kind;
    size_t j = 0;
    for (; j < seen_n; ++j) {
      if (strcmp(seen[j], kind) == 0) {
        counts[j]++;
        break;
      }
    }
    if (j == seen_n && seen_n < 32) {
      seen[seen_n] = kind;
      counts[seen_n] = 1;
      seen_n++;
    }
  }
  for (size_t i = 0; i < seen_n; ++i) {
    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "kind", seen[i]);
    cJSON_AddNumberToObject(row, "count", counts[i]);
    cJSON_AddItemToArray(kinds, row);
  }
  queue_event(session, "grid.ledger_pruned", payload);
  queue_text(session, "Ambient traces pruned (%zu -> %zu).", before,
             server->trace_count);
}

static void free_holding_pit(hg_session *session, hg_server *server) {
  if (!warden_cleared(server)) {
    queue_text(session,
               "The warden bars your way, keys jangling. Defeat it first.");
    return;
  }
  if (hg_character_has_item(&session->character, "antidote")) {
    queue_text(session,
               "The maiden smiles weakly. \"You already carry my vial. Use it "
               "well.\"");
    return;
  }
  char freed[1][HG_REFUGEE_NAME_LEN];
  pick_refugee_names(freed, 1);
  hg_character_add_item(&session->character, "antidote");
  shift_morality(session, 12);
  add_deed(server, session->character.name, "freed");
  hg_store_save(&server->store, &session->character);
  emit_rescued(session, server, freed, 1);
  queue_text(session,
             "You strike the chains free. The captive presses a vial into your "
             "hands:");
  queue_text(session,
             "  \"Antivenom, for the poison that haunts these wastes. My name "
             "is %s. I won't forget yours.\"",
             freed[0]);
  broadcast_room(server, "holding_pit", session->character.name,
                 "%s frees %s from the holding pit!", session->character.name,
                 freed[0]);
  char trace[160];
  snprintf(trace, sizeof(trace), "%s cut %s loose from the holding pit.",
           session->character.name, freed[0]);
  record_trace(server, server->config->world_name, "holding_pit", "quest",
               trace);
  moral_arc(session, server);
  send_affects(session);
  const hg_room *room = hg_world_room("holding_pit");
  if (room != NULL) {
    send_actions(session, server, room);
  }
}

static void free_cells(hg_session *session, hg_server *server) {
  if (!cages_ready(server, "cells")) {
    queue_text(session,
               "The cages stand open and empty; someone already cut them "
               "loose. The Front will round up more soon enough -- it always "
               "does -- but not yet.");
    return;
  }
  size_t count = (size_t)(rand() % 2) + 2;
  char freed[4][HG_REFUGEE_NAME_LEN];
  pick_refugee_names(freed, count);
  set_cage_refill(server, "cells");
  shift_morality(session, 15);
  add_deed(server, session->character.name, "freed");
  hg_store_save(&server->store, &session->character);
  emit_rescued(session, server, freed, count);
  char listed[160];
  format_name_list(listed, sizeof(listed), freed, count);
  queue_text(session,
             "You wrench the cages open. %s stumble out into the dark, some "
             "pausing only to grip your hand on the way past. Whatever else "
             "you are, whatever else you've done -- you did this.",
             listed);
  broadcast_room(server, "cells", session->character.name,
                 "%s throws open the Front's cages!", session->character.name);
  record_trace(server, server->config->world_name, "cells", "quest",
               "someone freed the caged refugees here.");
  moral_arc(session, server);
  send_affects(session);
}

static void cmd_free_captive(hg_session *session, hg_server *server) {
  if (strcmp(session->character.room, "cells") == 0) {
    free_cells(session, server);
    return;
  }
  if (strcmp(session->character.room, "holding_pit") == 0) {
    free_holding_pit(session, server);
    return;
  }
  queue_text(session, "There is no one here to free.");
}

static void cmd_shelter(hg_session *session, hg_server *server) {
  if (strcmp(session->character.room, "transit_hub") != 0) {
    queue_text(session,
               "There's no one here to shelter. The distress call comes from "
               "the old transit hub, south off the Scorch Road.");
    return;
  }
  if (!cages_ready(server, "transit_hub")) {
    queue_text(session,
               "The platform is empty now. Whoever called, you got them "
               "moving -- toward the free camp, you have to believe. The Front "
               "will strand others here soon enough; it always does, and the "
               "call will go out again.");
    return;
  }
  size_t count = (size_t)(rand() % 2) + 2;
  char saved[4][HG_REFUGEE_NAME_LEN];
  pick_refugee_names(saved, count);
  set_cage_refill(server, "transit_hub");
  shift_morality(session, 15);
  add_deed(server, session->character.name, "sheltered");
  hg_store_save(&server->store, &session->character);
  emit_rescued(session, server, saved, count);
  char listed[160];
  format_name_list(listed, sizeof(listed), saved, count);
  queue_text(session,
             "You answer the call. You get %s up and moving -- bottles filled "
             "at the tap, the youngest carried -- and stand watch on the "
             "cracked platform while they slip out the far side, toward the "
             "free camp and whatever the free folk can spare. The hand-radio "
             "goes quiet at last. Someone came.",
             listed);
  broadcast_room(server, "transit_hub", session->character.name,
                 "%s gets the stranded survivors moving toward safety.",
                 session->character.name);
  record_trace(server, server->config->world_name, "transit_hub", "aid",
               "someone answered the transit-hub distress call.");
  moral_arc(session, server);
  send_affects(session);
}

static void cmd_saved(hg_session *session, hg_server *server) {
  cJSON *payload = json_object();
  cJSON *roll = cJSON_AddArrayToObject(payload, "rescued");
  size_t limit = server->rescued_count;
  if (limit > 12) {
    limit = 12;
  }
  if (limit == 0) {
    queue_text(session,
               "No one has been pulled from the cages yet, or the Grid has "
               "forgotten. Find the Front's cages and change that.");
  } else {
    queue_text(session, "The Grid keeps these, pulled back out of the cages:");
    for (size_t i = 0; i < limit; ++i) {
      const hg_rescued_entry *entry = &server->rescued[i];
      if (strcmp(entry->world, server->config->world_name) != 0) {
        queue_text(session, "  %s  -- freed by %s, on %s", entry->name,
                   entry->saved_by, entry->world);
      } else {
        queue_text(session, "  %s  -- freed by %s", entry->name,
                   entry->saved_by);
      }
      cJSON *row = cJSON_CreateObject();
      cJSON_AddStringToObject(row, "name", entry->name);
      cJSON_AddStringToObject(row, "savedBy", entry->saved_by);
      cJSON_AddStringToObject(row, "world", entry->world);
      cJSON_AddNumberToObject(row, "at", (double)entry->at * 1000.0);
      cJSON_AddItemToArray(roll, row);
    }
  }
  queue_event(session, "grid.rescued_roll", payload);
}

static void cmd_sleep(hg_session *session, hg_server *server) {
  if (session->target[0] != '\0') {
    queue_text(session, "Not while you're fighting for your life.");
    return;
  }
  snprintf(session->character.position, sizeof(session->character.position),
           "resting");
  queue_text(session,
             "You close your eyes, and the dead network leans close and shows "
             "you something.");
  send_dream(session, server);
  send_vitals(session);
}

static void cmd_stand(hg_session *session) {
  snprintf(session->character.position, sizeof(session->character.position),
           "standing");
  queue_text(session, "You get to your feet.");
  send_vitals(session);
}

#define HG_DUST_COST 10

static int contains_ci(const char *haystack, const char *needle) {
  if (haystack == NULL || needle == NULL || needle[0] == '\0') {
    return 0;
  }
  for (const char *h = haystack; *h != '\0'; ++h) {
    const char *a = h;
    const char *b = needle;
    while (*a != '\0' && *b != '\0' &&
           tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
      ++a;
      ++b;
    }
    if (*b == '\0') {
      return 1;
    }
  }
  return 0;
}

typedef struct {
  const char *id;
  int price;
} hg_tinker_stock;

static const hg_tinker_stock k_tinker_stock[] = {
    {"helm", 14},
    {"plating", 18},
    {"rebar", 20},
};
static const size_t k_tinker_stock_count =
    sizeof(k_tinker_stock) / sizeof(k_tinker_stock[0]);

static int find_inventory(const hg_character *character, const char *arg,
                          char *out_id, size_t out_size) {
  if (character == NULL || arg == NULL || arg[0] == '\0') {
    return -1;
  }
  for (size_t i = 0; i < character->inventory_count; ++i) {
    if (strcasecmp(character->inventory[i], arg) == 0) {
      snprintf(out_id, out_size, "%s", character->inventory[i]);
      return 0;
    }
  }
  for (size_t i = 0; i < character->inventory_count; ++i) {
    const char *name = hg_item_name(character->inventory[i]);
    if (name != NULL && contains_ci(name, arg)) {
      snprintf(out_id, out_size, "%s", character->inventory[i]);
      return 0;
    }
  }
  return -1;
}

static int tinker_price(const char *arg, char *out_id, size_t out_size) {
  if (arg == NULL || arg[0] == '\0') {
    return -1;
  }
  for (size_t i = 0; i < k_tinker_stock_count; ++i) {
    if (strcasecmp(k_tinker_stock[i].id, arg) == 0) {
      snprintf(out_id, out_size, "%s", k_tinker_stock[i].id);
      return k_tinker_stock[i].price;
    }
    const char *name = hg_item_name(k_tinker_stock[i].id);
    if (name != NULL && contains_ci(name, arg)) {
      snprintf(out_id, out_size, "%s", k_tinker_stock[i].id);
      return k_tinker_stock[i].price;
    }
  }
  return -1;
}

static void cmd_list(hg_session *session) {
  if (strcmp(session->character.room, "workshop") != 0) {
    queue_text(session, "There is no one here selling anything.");
    return;
  }
  queue_text(session, "The tinker's wares, laid out on an oily cloth:");
  for (size_t i = 0; i < k_tinker_stock_count; ++i) {
    queue_text(session, "  %s -- %d gold", hg_item_name(k_tinker_stock[i].id),
               k_tinker_stock[i].price);
  }
}

static void cmd_buy(hg_session *session, hg_server *server, const char *arg) {
  if (strcmp(session->character.room, "tavern") == 0) {
    if (arg == NULL || !contains_ci(arg, "dust")) {
      queue_text(session, "The dealer only deals one thing: dust. (\"buy dust\")");
      return;
    }
    if (session->character.gold < HG_DUST_COST) {
      queue_text(session, "The dealer sneers. \"%d gold, no credit.\" You're short.",
                 HG_DUST_COST);
      return;
    }
    session->character.gold -= HG_DUST_COST;
    hg_character_add_item(&session->character, "dust");
    hg_store_save(&server->store, &session->character);
    queue_text(session,
               "The dealer slips you a packet of dust. (-%d gold, gold: %d)",
               HG_DUST_COST, session->character.gold);
    send_vitals(session);
    return;
  }
  if (strcmp(session->character.room, "workshop") != 0) {
    queue_text(session, "There is nothing to buy here.");
    return;
  }
  char item_id[16];
  int price = tinker_price(arg, item_id, sizeof(item_id));
  if (price < 0) {
    queue_text(session, "The tinker doesn't sell that.");
    return;
  }
  if (session->character.gold < price) {
    queue_text(session,
               "You can't afford that -- it is %d gold and you have %d.", price,
               session->character.gold);
    return;
  }
  if (hg_character_add_item(&session->character, item_id) != 0) {
    queue_text(session, "Your pack is full.");
    return;
  }
  session->character.gold -= price;
  hg_store_save(&server->store, &session->character);
  queue_text(session, "The tinker hands you %s and pockets your coin.",
             hg_item_name(item_id));
  send_vitals(session);
}

static void cmd_sell_item(hg_session *session, hg_server *server,
                          const char *arg) {
  if (strcmp(session->character.room, "market") != 0) {
    queue_text(session, "You can't do that here.");
    return;
  }
  if (strcmp(session->character.faction, "front") == 0) {
    queue_text(session,
               "The vendor drone's optic flares red. \"Cinder Front. We "
               "remember Scrap Market. We don't trade with your kind.\" It "
               "turns its back on you, and the stalls nearby go quiet.");
    return;
  }
  if (arg == NULL || arg[0] == '\0') {
    queue_text(session, "Sell what?");
    return;
  }
  char item_id[16];
  if (find_inventory(&session->character, arg, item_id, sizeof(item_id)) != 0) {
    queue_text(session, "You aren't carrying \"%s\".", arg);
    return;
  }
  const hg_item *item = hg_item_by_id(item_id);
  int base = item != NULL ? item->value : 0;
  if (base <= 0) {
    queue_text(session, "The vendor drone won't touch %s.", hg_item_name(item_id));
    return;
  }
  int value = base;
  if (strcmp(session->character.faction, "ally") == 0) {
    value = (base * 12) / 10;
  }
  hg_character_remove_item(&session->character, item_id);
  session->character.gold += value;
  hg_store_save(&server->store, &session->character);
  if (value > base) {
    queue_text(session, "You sell %s for %d gold. (the elves see you right)",
               hg_item_name(item_id), value);
  } else {
    queue_text(session, "You sell %s for %d gold.", hg_item_name(item_id),
               value);
  }
  send_vitals(session);
}

static void cmd_use(hg_session *session, hg_server *server, const char *arg) {
  if (arg == NULL || arg[0] == '\0') {
    queue_text(session, "Use what?");
    return;
  }
  char item_id[16];
  if (find_inventory(&session->character, arg, item_id, sizeof(item_id)) != 0) {
    queue_text(session, "You aren't carrying \"%s\".", arg);
    return;
  }
  if (strcmp(item_id, "dust") == 0) {
    hg_character_remove_item(&session->character, "dust");
    session->character.hp = session->character.max_hp;
    session->character.morality -= 10;
    session->character.addiction += 1;
    hg_store_save(&server->store, &session->character);
    queue_text(session,
               "The dust hits like a sunrise behind your eyes. Pain forgotten, "
               "body humming, you feel whole again. (HP %d/%d)",
               session->character.hp, session->character.max_hp);
    if (session->character.addiction >= 3) {
      queue_text(session,
                 "But the wanting is louder now. Your hands won't stop shaking "
                 "when it fades.");
    }
    send_vitals(session);
    send_affects(session);
    return;
  }
  if (strcmp(item_id, "antidote") == 0) {
    queue_text(session, "You aren't poisoned. Best to save it.");
    return;
  }
  queue_text(session, "You can't figure out how to use %s.",
             hg_item_name(item_id));
}

static void cmd_talk(hg_session *session, hg_server *server) {
  const char *room = session->character.room;
  (void)server;
  if (strcmp(room, "tavern") == 0) {
    queue_text(session,
               "The dealer rolls a packet of dust between his fingers: \"First "
               "taste eases any pain, friend. Just say buy dust.\"");
    queue_text(session,
               "Across the room the tavern wench catches your eye and tilts "
               "her head toward the back rooms.");
    queue_text(session, "(You could buy/use dust, carouse, or resist.)");
    return;
  }
  if (strcmp(room, "workshop") == 0) {
    queue_text(session,
               "The tinker doesn't look up from their soldering. \"Salvage's on "
               "the racks, prices on the list. Say 'list', say 'buy'. I don't "
               "haggle and I don't chat.\"");
    return;
  }
  if (strcmp(room, "market") == 0) {
    if (strcmp(session->character.faction, "none") == 0) {
      queue_text(session,
                 "A Cinder Front recruiter bellows from a crate: \"The wastes "
                 "are OURS! Round up every unregistered elf and drive them "
                 "out!\"");
      queue_text(session,
                 "A frightened elf refugee murmurs at your side: \"Please, I "
                 "was born here. Don't let them take me.\"");
      queue_text(session, "(You could join the Front, or defend the refugees.)");
    } else if (strcmp(session->character.faction, "front") == 0) {
      queue_text(session,
                 "The recruiter nods at you, one of his own now. The square has "
                 "gone quiet and afraid.");
    } else {
      queue_text(session,
                 "An elf refugee presses your hand in silent thanks. The "
                 "recruiter is nowhere in sight.");
    }
    return;
  }
  if (strcmp(room, "dais") == 0) {
    if (strcmp(session->character.faction, "ally") == 0) {
      queue_text(session,
                 "The Ashmonger laughs, low and delighted. \"The elf-lover "
                 "walked right into my house. Bold. I am going to wear you as a "
                 "banner.\" There is no talking your way out of this -- only "
                 "steel.");
    } else if (strcmp(session->character.faction, "front") == 0) {
      queue_text(session,
                 "The Ashmonger claps a heavy hand on your shoulder. \"You came "
                 "far for the cause. Kneel and take your place at my right hand "
                 "-- or find your spine and 'defy' me, here and now. Choose "
                 "what you are.\"");
    } else {
      queue_text(session,
                 "The Ashmonger spits. \"Pledge to the Front or get off my "
                 "dais. I have no patience for fence-sitters.\"");
    }
    return;
  }
  if (strcmp(room, "floodgate") == 0) {
    queue_text(session,
               "A stranded operator looks up from a dead console: \"I can't "
               "leave until this node is restored, and the Custodian dragged "
               "the core shard down into the Core Lab.\"");
    return;
  }
  if (strcmp(room, "waystation") == 0) {
    queue_text(session,
               "The medic studies you. \"We tend friends of the free folk. Pick "
               "a side, wanderer, and we will see.\"");
    return;
  }
  queue_text(session, "You can't do that here.");
}

static void cmd_title(hg_session *session, hg_server *server, const char *arg) {
  if (arg == NULL || arg[0] == '\0') {
    session->character.title[0] = '\0';
    queue_text(session, "Your title is cleared.");
  } else {
    snprintf(session->character.title, sizeof(session->character.title), "%s",
             arg);
    queue_text(session, "Your title is now: %s.", session->character.title);
  }
  hg_store_save(&server->store, &session->character);
}

static void cmd_who(hg_session *session, hg_server *server) {
  cJSON *payload = json_object();
  cJSON *players = cJSON_AddArrayToObject(payload, "players");
  char line[512] = "Online:";
  size_t used = strlen(line);
  int any = 0;
  for (hg_session *other = server->sessions; other != NULL; other = other->next) {
    if (other->state != HG_PLAYING) {
      continue;
    }
    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "world", server->config->world_name);
    cJSON_AddStringToObject(row, "name", other->character.name);
    cJSON_AddStringToObject(row, "regard", player_regard(&other->character));
    cJSON_AddBoolToObject(row, "here", 1);
    cJSON_AddStringToObject(row, "title", other->character.title);
    cJSON_AddItemToArray(players, row);

    char entry[96];
    if (other->character.title[0] != '\0') {
      snprintf(entry, sizeof(entry), "%s %s", other->character.name,
               other->character.title);
    } else {
      snprintf(entry, sizeof(entry), "%s", other->character.name);
    }
    const char *regard = player_regard(&other->character);
    if (regard[0] != '\0') {
      size_t entry_len = strlen(entry);
      snprintf(entry + entry_len, sizeof(entry) - entry_len, " (%s)", regard);
    }
    int written =
        snprintf(line + used, sizeof(line) - used, "%s %s", any ? ";" : "",
                 entry);
    if (written > 0 && (size_t)written < sizeof(line) - used) {
      used += (size_t)written;
    }
    any = 1;
  }
  queue_event(session, "grid.who", payload);
  if (!any) {
    queue_text(session, "No one else walks the wastes right now.");
  } else {
    queue_text(session, "%s.", line);
  }
}

static void cmd_ability(hg_session *session, hg_server *server) {
  time_t now = time(NULL);
  if (session->trait_ready_at > now) {
    long left = (long)(session->trait_ready_at - now);
    if (left < 1) {
      left = 1;
    }
    queue_text(session, "Requisition is still recharging. (%lds)", left);
    return;
  }
  if (strcmp(session->character.race, "human") == 0) {
    int coin = 15 + (rand() % 16);
    session->character.gold += coin;
    session->trait_ready_at = now + 180;
    hg_store_save(&server->store, &session->character);
    queue_text(session,
               "You flash credentials nobody bothers to check. The registry "
               "still provides for its own. (+%d gold)",
               coin);
    send_vitals(session);
    return;
  }
  if (strcmp(session->character.race, "ghoul") == 0) {
    session->character.hp += 25;
    if (session->character.hp > session->character.max_hp) {
      session->character.hp = session->character.max_hp;
    }
    session->trait_ready_at = now + 120;
    queue_text(session, "Rad-scoured flesh knits itself shut. (+25 hp)");
    send_vitals(session);
    return;
  }
  if (strcmp(session->character.race, "revenant") == 0) {
    session->character.hp += 15;
    if (session->character.hp > session->character.max_hp) {
      session->character.hp = session->character.max_hp;
    }
    session->trait_ready_at = now + 120;
    queue_text(session,
               "You reach into the dead Grid and draw back a little of its cold "
               "life. (+15 hp)");
    send_vitals(session);
    return;
  }
  if (strcmp(session->character.race, "vatborn") == 0) {
    session->character.hp += 12;
    if (session->character.hp > session->character.max_hp) {
      session->character.hp = session->character.max_hp;
    }
    session->trait_ready_at = now + 120;
    queue_text(session,
               "You print a field stim from raw salvage and jab it home. (+12 "
               "hp)");
    send_vitals(session);
    return;
  }
  if (strcmp(session->character.race, "dustkin") == 0) {
    const hg_room *room = hg_world_room(session->character.room);
    if (room == NULL || !room->outdoors) {
      queue_text(session,
                 "Nothing to forage in here. You need the open wastes under the "
                 "sky.");
      return;
    }
    int coin = 5 + (rand() % 11);
    session->character.gold += coin;
    session->trait_ready_at = now + 90;
    hg_store_save(&server->store, &session->character);
    queue_text(session,
               "You work the open pan and turn up something worth keeping. (+%d "
               "gold)",
               coin);
    send_vitals(session);
    return;
  }
  if (strcmp(session->character.race, "chromed") == 0) {
    queue_text(session,
               "You spin your augments up to a scream, but there's nothing here "
               "to dump the charge into.");
    return;
  }
  if (strcmp(session->character.race, "elf") == 0) {
    queue_text(session,
               "You ready to slip the net, but there is no fight here to vanish "
               "from.");
    return;
  }
  queue_text(session, "Nothing answers that call.");
}

static void handle_play(hg_session *session, hg_server *server, char *input) {
  hg_world_tick_respawns(&server->world);
  char command[160];
  snprintf(command, sizeof(command), "%s", input);
  char *verb = trim(command);
  char *arg = NULL;
  char *space = strchr(verb, ' ');
  if (space != NULL) {
    *space = '\0';
    arg = trim(space + 1);
  }
  for (char *p = verb; *p != '\0'; ++p) {
    *p = (char)tolower((unsigned char)*p);
  }
  if (strcmp(verb, "go") == 0 && arg != NULL) {
    verb = arg;
    arg = NULL;
    for (char *p = verb; *p != '\0'; ++p) {
      *p = (char)tolower((unsigned char)*p);
    }
  }

  if (strcmp(verb, "look") == 0 || strcmp(verb, "l") == 0) {
    if (arg != NULL && arg[0] != '\0') {
      hg_live_mob *mob =
          hg_world_mob_in_room(&server->world, session->character.room, arg);
      if (mob != NULL) {
        queue_text(session, "%s", mob->description);
        return;
      }
      hg_session *other = session_find(server, arg);
      if (other != NULL &&
          strcmp(other->character.room, session->character.room) == 0) {
        char tagged[128];
        const char *brand = player_brand(&other->character);
        if (other->character.title[0] != '\0' && brand[0] != '\0') {
          snprintf(tagged, sizeof(tagged), "%s, %s (%s)", other->character.name,
                   other->character.title, brand);
        } else if (other->character.title[0] != '\0') {
          snprintf(tagged, sizeof(tagged), "%s, %s", other->character.name,
                   other->character.title);
        } else if (brand[0] != '\0') {
          snprintf(tagged, sizeof(tagged), "%s (%s)", other->character.name,
                   brand);
        } else {
          snprintf(tagged, sizeof(tagged), "%s", other->character.name);
        }
        queue_text(session, "%s stands before you, looking steady.", tagged);
        cJSON *payload = json_object();
        cJSON_AddStringToObject(payload, "name", other->character.name);
        cJSON_AddStringToObject(payload, "title", other->character.title);
        cJSON_AddStringToObject(payload, "faction", other->character.faction);
        cJSON_AddBoolToObject(payload, "ashsworn", other->character.ashsworn);
        cJSON_AddStringToObject(payload, "regard",
                                player_regard(&other->character));
        queue_event(session, "player.read", payload);
        return;
      }
      queue_text(session, "You don't see that here.");
      return;
    }
    send_scene(session, server);
    return;
  }
  if (strcmp(verb, "exits") == 0) {
    const hg_room *room = hg_world_room(session->character.room);
    if (room != NULL) {
      char line[160] = "Exits:";
      size_t used = strlen(line);
      for (size_t i = 0; i < room->exit_count; ++i) {
        int written = snprintf(line + used, sizeof(line) - used, " %s",
                               room->exits[i].direction);
        if (written < 0 || (size_t)written >= sizeof(line) - used) {
          break;
        }
        used += (size_t)written;
      }
      queue_text(session, "%s", line);
    }
    return;
  }
  if (strcmp(verb, "inventory") == 0 || strcmp(verb, "inv") == 0 ||
      strcmp(verb, "i") == 0) {
    cmd_inventory(session);
    return;
  }
  if (strcmp(verb, "wield") == 0 || strcmp(verb, "wear") == 0 ||
      strcmp(verb, "equip") == 0) {
    if (arg == NULL || !hg_character_has_item(&session->character, arg)) {
      queue_text(session, "You have nothing like that to wear.");
      return;
    }
    const hg_item *item = hg_item_by_id(arg);
    if (item == NULL || item->slot == NULL ||
        strcmp(item->slot, "weapon") != 0) {
      queue_text(session, "You have nothing like that to wear.");
      return;
    }
    snprintf(session->character.weapon, sizeof(session->character.weapon), "%s",
             arg);
    queue_text(session, "You ready %s.", item->name);
    send_equipment(session);
    hg_store_save(&server->store, &session->character);
    return;
  }
  if (strcmp(verb, "remove") == 0 || strcmp(verb, "unwield") == 0) {
    if (arg == NULL || strcmp(session->character.weapon, arg) != 0) {
      queue_text(session, "You are not wearing that.");
      return;
    }
    session->character.weapon[0] = '\0';
    queue_text(session, "You put %s away.", hg_item_name(arg));
    send_equipment(session);
    hg_store_save(&server->store, &session->character);
    return;
  }
  if (strcmp(verb, "consider") == 0 || strcmp(verb, "con") == 0) {
    if (arg == NULL) {
      queue_text(session, "There's nothing like that here to size up.");
      return;
    }
    hg_live_mob *mob =
        hg_world_mob_in_room(&server->world, session->character.room, arg);
    if (mob == NULL) {
      queue_text(session, "There's nothing like that here to size up.");
      return;
    }
    char line[192];
    snprintf(line, sizeof(line), consider_line(&session->character, mob),
             mob->name);
    queue_text(session, "%s", line);
    return;
  }
  if (strcmp(verb, "attack") == 0 || strcmp(verb, "kill") == 0 ||
      strcmp(verb, "k") == 0) {
    if (session->target[0] != '\0') {
      queue_text(session, "You're already locked in this fight.");
      return;
    }
    hg_live_mob *live[HG_MAX_LIVE_MOBS];
    size_t count = hg_world_mobs_in_room(&server->world, session->character.room,
                                         live, HG_MAX_LIVE_MOBS);
    hg_live_mob *mob = NULL;
    if (arg != NULL && arg[0] != '\0') {
      mob = hg_world_mob_in_room(&server->world, session->character.room, arg);
    }
    if (mob == NULL) {
      if (count > 0) {
        char names[160] = "";
        size_t used = 0;
        for (size_t i = 0; i < count; ++i) {
          int written =
              snprintf(names + used, sizeof(names) - used, "%s%s",
                       i == 0 ? "" : ", ", live[i]->name);
          if (written < 0 || (size_t)written >= sizeof(names) - used) {
            break;
          }
          used += (size_t)written;
        }
        queue_text(session,
                   "There's nothing like %s to fight here. You could try: %s.",
                   arg != NULL && arg[0] != '\0' ? arg : "that", names);
      } else {
        queue_text(session, "There's nothing like that here to attack.");
      }
      return;
    }
    snprintf(session->character.position, sizeof(session->character.position),
             "standing");
    snprintf(session->target, sizeof(session->target), "%s", mob->id);
    cJSON *start = json_object();
    cJSON_AddStringToObject(start, "mob", mob->id);
    cJSON_AddStringToObject(start, "name", mob->name);
    queue_event(session, "combat.start", start);
    queue_text(session, "You throw yourself at %s.", mob->name);
    send_vitals(session);
    return;
  }
  if (strcmp(verb, "rest") == 0) {
    if (session->target[0] != '\0') {
      queue_text(session, "Not while you're fighting for your life.");
      return;
    }
    snprintf(session->character.position, sizeof(session->character.position),
             "resting");
    queue_text(session, "You settle in and rest.");
    send_vitals(session);
    return;
  }
  if (strcmp(verb, "sleep") == 0) {
    cmd_sleep(session, server);
    return;
  }
  if (strcmp(verb, "stand") == 0 || strcmp(verb, "wake") == 0) {
    cmd_stand(session);
    return;
  }
  if (strcmp(verb, "free") == 0 || strcmp(verb, "rescue") == 0 ||
      strcmp(verb, "release") == 0 || strcmp(verb, "unlock") == 0 ||
      strcmp(verb, "liberate") == 0 || strcmp(verb, "unchain") == 0 ||
      strcmp(verb, "unshackle") == 0 || strcmp(verb, "untie") == 0) {
    cmd_free_captive(session, server);
    return;
  }
  if (strcmp(verb, "shelter") == 0 || strcmp(verb, "guide") == 0) {
    cmd_shelter(session, server);
    return;
  }
  if (strcmp(verb, "saved") == 0 || strcmp(verb, "rescued") == 0 ||
      strcmp(verb, "roll") == 0) {
    cmd_saved(session, server);
    return;
  }
  if (strcmp(verb, "sense") == 0 || strcmp(verb, "actions") == 0) {
    const hg_room *room = hg_world_room(session->character.room);
    if (room != NULL) {
      send_actions(session, server, room);
    }
    return;
  }
  if (strcmp(verb, "join") == 0) {
    cmd_join(session, server);
    return;
  }
  if (strcmp(verb, "defend") == 0) {
    cmd_defend(session, server);
    return;
  }
  if (strcmp(verb, "defy") == 0) {
    dais_defect(session, server);
    return;
  }
  if (strcmp(verb, "forgive") == 0 || strcmp(verb, "absolve") == 0 ||
      strcmp(verb, "pardon") == 0) {
    cmd_forgive(session, server, arg);
    return;
  }
  if (strcmp(verb, "reckoning") == 0) {
    cmd_reckoning(session, server);
    return;
  }
  if (strcmp(verb, "war") == 0) {
    cmd_war(session, server);
    return;
  }
  if (strcmp(verb, "wall") == 0) {
    cmd_wall(session, server, arg != NULL ? arg : "");
    return;
  }
  if (strcmp(verb, "cache") == 0) {
    cmd_cache(session, server, arg);
    return;
  }
  if (strcmp(verb, "gather") == 0) {
    cmd_gather(session, server);
    return;
  }
  if (strcmp(verb, "give") == 0) {
    cmd_give(session, server, arg);
    return;
  }
  if (strcmp(verb, "treat") == 0) {
    cmd_treat(session, server);
    return;
  }
  if (strcmp(verb, "witness") == 0) {
    cmd_witness(session, server, arg != NULL ? arg : "");
    return;
  }
  if (strcmp(verb, "gridstats") == 0) {
    cmd_gridstats(session, server);
    return;
  }
  if (strcmp(verb, "gridprune") == 0) {
    cmd_gridprune(session, server);
    return;
  }
  if (strcmp(verb, "affects") == 0) {
    send_affects(session);
    queue_text(session, "You stand clear: no afflictions hold you.");
    return;
  }
  if (strcmp(verb, "listen") == 0) {
    if (server->trace_count > 0) {
      const hg_trace *trace =
          &server->traces[(size_t)rand() % server->trace_count];
      cJSON *payload = json_object();
      cJSON_AddStringToObject(payload, "kind", "echo");
      cJSON_AddStringToObject(payload, "text", trace->text);
      queue_event(session, "grid.transmission", payload);
      queue_text(session,
                 "You go still and tune the dead frequencies. The static "
                 "thins, and the network plays something back -- a memory it "
                 "never let go of:");
      queue_text(session, "  >> %s <<", trace->text);
      return;
    }
    cJSON *payload = json_object();
    cJSON_AddStringToObject(payload, "kind", "signal");
    cJSON_AddStringToObject(
        payload, "text",
        "a residual field hums through dead ferrite, carrying a half-erased name");
    queue_event(session, "grid.transmission", payload);
    queue_text(session,
               "You go still and tune the dead frequencies. Something answers:");
    queue_text(session,
               "  >> a residual field hums through dead ferrite, carrying a "
               "half-erased name <<");
    return;
  }
  if (strcmp(verb, "ping") == 0) {
    if (arg != NULL && strcmp(arg, "all") == 0) {
      cJSON *payload = json_object();
      cJSON *traces = cJSON_AddArrayToObject(payload, "traces");
      for (size_t i = 0; i < server->trace_count; ++i) {
        cJSON *trace = cJSON_CreateObject();
        cJSON_AddStringToObject(trace, "world", server->traces[i].world);
        cJSON_AddStringToObject(trace, "node", server->traces[i].node);
        cJSON_AddStringToObject(trace, "kind", server->traces[i].kind);
        cJSON_AddStringToObject(trace, "text", server->traces[i].text);
        cJSON_AddItemToArray(traces, trace);
      }
      queue_event(session, "grid.federation", payload);
      queue_text(session, "Federation echoes return.");
      return;
    }
    cJSON *payload = json_object();
    cJSON_AddStringToObject(payload, "node", session->character.room);
    cJSON *traces = cJSON_AddArrayToObject(payload, "traces");
    for (size_t i = 0; i < server->trace_count; ++i) {
      if (strcmp(server->traces[i].node, session->character.room) != 0) {
        continue;
      }
      cJSON *trace = cJSON_CreateObject();
      cJSON_AddStringToObject(trace, "world", server->traces[i].world);
      cJSON_AddStringToObject(trace, "node", server->traces[i].node);
      cJSON_AddStringToObject(trace, "kind", server->traces[i].kind);
      cJSON_AddStringToObject(trace, "text", server->traces[i].text);
      cJSON_AddItemToArray(traces, trace);
    }
    queue_event(session, "grid.echo", payload);
    queue_text(session, "This node answers.");
    return;
  }
  if (strcmp(verb, "inscribe") == 0 || strcmp(verb, "carve") == 0) {
    if (arg == NULL || arg[0] == '\0') {
      queue_text(session, "Inscribe what?");
      return;
    }
    record_trace(server, server->config->world_name, session->character.room,
                 "mark", arg);
    cJSON *payload = json_object();
    cJSON_AddStringToObject(payload, "node", session->character.room);
    cJSON_AddStringToObject(payload, "text", arg);
    queue_event(session, "grid.inscribed", payload);
    queue_text(session, "You carve it into the node.");
    return;
  }
  if (strcmp(verb, "help") == 0) {
    queue_text(session,
               "Commands: look, exits, inventory, wield, remove, consider, "
               "attack, rest, sleep, list, buy, sell, use, talk, ability, "
               "title, who, join, defend, free, shelter, saved, listen, ping, "
               "wall, cache, gather, give, treat, witness, tell, reply, yell, "
               "emote, sense, help.");
    return;
  }
  if (strcmp(verb, "list") == 0 || strcmp(verb, "wares") == 0) {
    cmd_list(session);
    return;
  }
  if (strcmp(verb, "buy") == 0) {
    cmd_buy(session, server, arg);
    return;
  }
  if (strcmp(verb, "use") == 0 || strcmp(verb, "drink") == 0 ||
      strcmp(verb, "eat") == 0) {
    cmd_use(session, server, arg);
    return;
  }
  if (strcmp(verb, "talk") == 0 || strcmp(verb, "ask") == 0) {
    cmd_talk(session, server);
    return;
  }
  if (strcmp(verb, "title") == 0) {
    cmd_title(session, server, arg != NULL ? arg : "");
    return;
  }
  if (strcmp(verb, "who") == 0) {
    cmd_who(session, server);
    return;
  }
  if (strcmp(verb, "ability") == 0 || strcmp(verb, "trait") == 0) {
    cmd_ability(session, server);
    return;
  }
  if (strcmp(verb, "tell") == 0) {
    if (arg == NULL || arg[0] == '\0') {
      queue_text(session, "Tell whom what?  (tell <player> <message>)");
      return;
    }
    char *message = strchr(arg, ' ');
    if (message == NULL || trim(message + 1)[0] == '\0') {
      queue_text(session, "Tell whom what?  (tell <player> <message>)");
      return;
    }
    *message = '\0';
    message = trim(message + 1);
    char target_name[33];
    snprintf(target_name, sizeof(target_name), "%s", trim(arg));
    hg_session *target = session_find(server, target_name);
    if (target == NULL) {
      queue_text(session, "No one by that name is connected.");
      return;
    }
    snprintf(target->reply_to, sizeof(target->reply_to), "%s",
             session->character.name);
    queue_text(target, "%s tells you, \"%s\"", session->character.name,
               message);
    cJSON *payload = json_object();
    cJSON_AddStringToObject(payload, "from", session->character.name);
    cJSON_AddStringToObject(payload, "text", message);
    queue_event(target, "comm.tell", payload);
    queue_text(session, "You tell %s, \"%s\"", target->character.name, message);
    return;
  }
  if (strcmp(verb, "reply") == 0) {
    if (session->reply_to[0] == '\0') {
      queue_text(session, "No one has told you anything lately.");
      return;
    }
    if (arg == NULL || arg[0] == '\0') {
      queue_text(session, "Reply with what?");
      return;
    }
    char composed[200];
    snprintf(composed, sizeof(composed), "%s %s", session->reply_to, arg);
    char *old_arg = arg;
    (void)old_arg;
    /* Reuse tell by reconstructing. */
    hg_session *target = session_find(server, session->reply_to);
    if (target == NULL) {
      queue_text(session, "No one by that name is connected.");
      return;
    }
    snprintf(target->reply_to, sizeof(target->reply_to), "%s",
             session->character.name);
    queue_text(target, "%s tells you, \"%s\"", session->character.name, arg);
    cJSON *payload = json_object();
    cJSON_AddStringToObject(payload, "from", session->character.name);
    cJSON_AddStringToObject(payload, "text", arg);
    queue_event(target, "comm.tell", payload);
    queue_text(session, "You tell %s, \"%s\"", target->character.name, arg);
    return;
  }
  if (strcmp(verb, "yell") == 0) {
    if (arg == NULL || arg[0] == '\0') {
      queue_text(session, "Yell what?  (yell <message>)");
      return;
    }
    for (hg_session *other = server->sessions; other != NULL;
         other = other->next) {
      if (other->state != HG_PLAYING) {
        continue;
      }
      if (other == session) {
        queue_text(other, "You yell, \"%s\"", arg);
      } else {
        queue_text(other, "%s yells, \"%s\"", session->character.name, arg);
      }
      cJSON *payload = json_object();
      cJSON_AddStringToObject(payload, "from", session->character.name);
      cJSON_AddStringToObject(payload, "text", arg);
      queue_event(other, "comm.yell", payload);
    }
    return;
  }
  if (strcmp(verb, "emote") == 0 || strcmp(verb, "em") == 0 ||
      strcmp(verb, "pose") == 0) {
    if (arg == NULL || arg[0] == '\0') {
      queue_text(session, "Emote what?  (emote <action>)");
      return;
    }
    queue_text(session, "%s %s", session->character.name, arg);
    for (hg_session *other = server->sessions; other != NULL;
         other = other->next) {
      if (other == session || other->state != HG_PLAYING) {
        continue;
      }
      if (strcmp(other->character.room, session->character.room) != 0) {
        continue;
      }
      queue_text(other, "%s %s", session->character.name, arg);
    }
    return;
  }
  if (strcmp(verb, "mend") == 0) {
    if (arg == NULL || arg[0] == '\0') {
      queue_text(session, "Mend whom?");
      return;
    }
    hg_session *other = session_find(server, arg);
    if (other == NULL ||
        strcmp(other->character.room, session->character.room) != 0) {
      queue_text(session, "No ally like that stands here.");
      return;
    }
    if (other->character.hp >= other->character.max_hp) {
      queue_text(session, "%s is already whole.", other->character.name);
      return;
    }
    queue_text(session, "You mend %s.", other->character.name);
    return;
  }
  if (strcmp(verb, "steal") == 0) {
    if (strcmp(session->character.room, "market") != 0) {
      queue_text(session, "You can't do that here.");
      return;
    }
    shift_morality(session, -8);
    session->character.gold += 12;
    add_deed(server, session->character.name, "stolen");
    hg_store_save(&server->store, &session->character);
    queue_text(session, "You snag a fistful of coin while the vendor looks away.");
    moral_arc(session, server);
    send_vitals(session);
    send_affects(session);
    return;
  }
  if (strcmp(verb, "sell") == 0 || strcmp(verb, "trade") == 0) {
    cmd_sell_item(session, server, arg);
    return;
  }
  if (strcmp(verb, "recall") == 0 || strcmp(verb, "home") == 0) {
    session->target[0] = '\0';
    snprintf(session->character.room, sizeof(session->character.room), "nexus");
    snprintf(session->character.position, sizeof(session->character.position),
             "standing");
    send_scene(session, server);
    return;
  }

  const hg_room *room = hg_world_room(session->character.room);
  const hg_room *destination = hg_world_move(room, verb);
  if (destination != NULL) {
    if (session->target[0] != '\0') {
      queue_text(session, "Not while you're fighting for your life.");
      return;
    }
    snprintf(session->character.room, sizeof(session->character.room), "%s",
             destination->id);
    snprintf(session->character.position, sizeof(session->character.position),
             "standing");
    hg_store_save(&server->store, &session->character);
    send_scene(session, server);
    return;
  }

  queue_text(session, "Nothing answers '%s'. Type help for verbs.", input);
}

static void handle_command(hg_session *session, hg_server *server,
                           char *input) {
  char *command = trim(input);
  if (command[0] == '\0') {
    return;
  }
  switch (session->state) {
  case HG_WAIT_NAME:
    handle_name(session, server, command);
    break;
  case HG_WAIT_RACE:
    handle_race(session, server, command);
    break;
  case HG_PLAYING:
    handle_play(session, server, command);
    break;
  }
}

static int send_http_json(struct lws *wsi, unsigned int status,
                          const char *body) {
  unsigned char headers[LWS_PRE + 512];
  unsigned char *start = &headers[LWS_PRE];
  unsigned char *p = start;
  unsigned char *end = &headers[sizeof(headers) - 1];
  size_t length = strlen(body);

  if (lws_add_http_common_headers(wsi, status, "application/json", length, &p,
                                  end) ||
      lws_finalize_write_http_header(wsi, start, &p, end)) {
    return -1;
  }
  if (lws_write_http(wsi, body, length) < 0) {
    return -1;
  }
  return lws_http_transaction_completed(wsi) ? -1 : 0;
}

static int handle_http(struct lws *wsi, hg_server *server, const char *path) {
  long long now = (long long)time(NULL) * 1000;
  char body[1024];
  if (strcmp(path, "/health") == 0) {
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"ts\":%lld,\"world\":\"%s\"}", now,
             server->config->world_name);
    return send_http_json(wsi, HTTP_STATUS_OK, body);
  }
  if (strcmp(path, "/health/deep") == 0) {
    snprintf(
        body, sizeof(body),
        "{\"ok\":true,\"ts\":%lld,\"world\":\"%s\",\"checks\":{"
        "\"world\":{\"ok\":true,\"latency_ms\":0,\"critical\":true},"
        "\"grid_hub\":{\"ok\":true,\"latency_ms\":0,\"critical\":false,"
        "\"mode\":\"local\"}}}",
        now, server->config->world_name);
    return send_http_json(wsi, HTTP_STATUS_OK, body);
  }
  if (strcmp(path, "/map.svg") == 0) {
    static const char *svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"720\" height=\"480\">"
        "<rect width=\"100%\" height=\"100%\" fill=\"#1a1410\"/>"
        "<text x=\"24\" y=\"36\" fill=\"#e8a060\" font-family=\"monospace\" "
        "font-size=\"18\">Ferrite Wastes</text>"
        "<text x=\"24\" y=\"70\" fill=\"#c8dde8\" font-family=\"monospace\" "
        "font-size=\"12\">nexus - workshop - roof - coil-yard</text>"
        "</svg>";
    unsigned char headers[LWS_PRE + 512];
    unsigned char *start = &headers[LWS_PRE];
    unsigned char *p = start;
    unsigned char *end = &headers[sizeof(headers) - 1];
    size_t length = strlen(svg);
    if (lws_add_http_common_headers(wsi, HTTP_STATUS_OK, "image/svg+xml",
                                    length, &p, end) ||
        lws_finalize_write_http_header(wsi, start, &p, end)) {
      return -1;
    }
    if (lws_write_http(wsi, svg, length) < 0) {
      return -1;
    }
    return lws_http_transaction_completed(wsi) ? -1 : 0;
  }
  snprintf(body, sizeof(body),
           "{\"ok\":false,\"error\":\"not found\",\"world\":\"%s\"}",
           server->config->world_name);
  return send_http_json(wsi, HTTP_STATUS_NOT_FOUND, body);
}

static int callback(struct lws *wsi, enum lws_callback_reasons reason,
                    void *user, void *in, size_t len) {
  hg_session *session = user;
  hg_server *server = lws_context_user(lws_get_context(wsi));

  switch (reason) {
  case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
    char path[128];
    int copied = lws_hdr_copy(wsi, path, sizeof(path), WSI_TOKEN_GET_URI);
    return copied > 0 && strcmp(path, "/ws") == 0 ? 0 : 1;
  }
  case LWS_CALLBACK_HTTP:
    return handle_http(wsi, server, (const char *)in);
  case LWS_CALLBACK_ESTABLISHED:
    memset(session, 0, sizeof(*session));
    session->wsi = wsi;
    session->state = HG_WAIT_NAME;
    queue_text(session, "\033[1;38;5;208mFERRITE WASTES\033[0m");
    queue_text(session, "The field is gone. What it changed remains.");
    queue_text(session, "By what name are you known, wanderer?");
    return 0;
  case LWS_CALLBACK_RECEIVE:
    if (session->input_length + len >= sizeof(session->input)) {
      session->input_length = 0;
      queue_text(session, "That command is too long.");
      return 0;
    }
    memcpy(session->input + session->input_length, in, len);
    session->input_length += len;
    if (lws_is_final_fragment(wsi)) {
      session->input[session->input_length] = '\0';
      handle_command(session, server, session->input);
      session->input_length = 0;
    }
    return 0;
  case LWS_CALLBACK_TIMER:
    if (session->state == HG_PLAYING) {
      on_tick(session, server);
    }
    return 0;
  case LWS_CALLBACK_SERVER_WRITEABLE:
    if (session->out_head != NULL) {
      hg_message *message = session->out_head;
      size_t length = strlen(message->text);
      unsigned char *buffer = malloc(LWS_PRE + length);
      if (buffer == NULL) {
        return -1;
      }
      memcpy(buffer + LWS_PRE, message->text, length);
      int written = lws_write(wsi, buffer + LWS_PRE, length, LWS_WRITE_TEXT);
      free(buffer);
      if (written < 0 || (size_t)written != length) {
        return -1;
      }
      session->out_head = message->next;
      if (session->out_head == NULL) {
        session->out_tail = NULL;
      }
      free(message->text);
      free(message);
      if (session->out_head != NULL) {
        lws_callback_on_writable(wsi);
      }
    }
    return 0;
  case LWS_CALLBACK_CLOSED:
    session_unregister(server, session);
    free_messages(session);
    return 0;
  default:
    return 0;
  }
}

static const struct lws_protocols protocols[] = {
    {"http", callback, sizeof(hg_session), 4096, 0, NULL, 0},
    LWS_PROTOCOL_LIST_TERM,
};

void hg_server_stop(void) { stop_requested = 1; }

int hg_server_run(const hg_server_config *config) {
  hg_server server = {
      .config = config,
      .started = time(NULL),
  };
  if (hg_store_init(&server.store, config->data_dir) != 0) {
    fprintf(stderr, "cannot initialize data directory: %s\n", config->data_dir);
    return 1;
  }
  parse_admins(&server, config->admins);
  hg_world_init(&server.world);
  seed_traces(&server);
  srand((unsigned)time(NULL));

  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.port = config->port;
  info.iface = config->host;
  info.protocols = protocols;
  info.user = &server;
  info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;

  lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
  server.context = lws_create_context(&info);
  if (server.context == NULL) {
    fprintf(stderr, "cannot create WebSocket server context\n");
    return 1;
  }

  fprintf(stdout, "%s listening on %s:%d\n", config->world_name, config->host,
          config->port);
  stop_requested = 0;
  while (!stop_requested) {
    if (lws_service(server.context, 50) < 0) {
      break;
    }
  }
  lws_context_destroy(server.context);
  return 0;
}
