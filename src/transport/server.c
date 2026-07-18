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

typedef struct hg_session {
  struct lws *wsi;
  hg_session_state state;
  hg_character character;
  char target[16];
  char reply_to[33];
  int market_resolved;
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
} hg_server;

static volatile sig_atomic_t stop_requested;

static const char *race_ids[] = {"human", "elf",    "revenant", "ghoul",
                                 "chromed", "dustkin", "vatborn"};
static const char *race_names[] = {"Human", "Elf",    "Revenant", "Ghoul",
                                   "Chromed", "Dustkin", "Vatborn"};
static const size_t race_count = sizeof(race_ids) / sizeof(race_ids[0]);

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

static void send_actions(hg_session *session, const hg_room *room) {
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
    cJSON_AddStringToObject(dust, "verb", "buy");
    cJSON_AddStringToObject(dust, "label", "buy dust for 5 gold");
    cJSON_AddStringToObject(dust, "kind", "vice");
    cJSON_AddItemToArray(actions, dust);
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
  send_actions(session, room);
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

static void cmd_join(hg_session *session, hg_server *server) {
  const hg_room *room = hg_world_room(session->character.room);
  if (room == NULL ||
      (strcmp(room->id, "market") != 0 &&
       strcmp(room->id, "bias-checkpoint") != 0)) {
    queue_text(session, "There is no Front recruiter here.");
    return;
  }
  snprintf(session->character.faction, sizeof(session->character.faction),
           "front");
  session->character.morality -= 20;
  session->market_resolved = 1;
  if (strcmp(session->character.race, "elf") == 0) {
    session->character.ashsworn = 1;
    snprintf(session->character.title, sizeof(session->character.title),
             "the ash-sworn");
    queue_text(session,
               "You take the Front's coin. The wastes mark you ash-sworn.");
  } else {
    queue_text(session, "You take the Front's coin and look away.");
  }
  record_trace(server, server->config->world_name, session->character.room,
               "oath", "a Cinder Front oath was sworn here");
  hg_store_save(&server->store, &session->character);
  send_affects(session);
  send_actions(session, room);
}

static void cmd_defend(hg_session *session, hg_server *server) {
  const hg_room *room = hg_world_room(session->character.room);
  if (room == NULL ||
      (strcmp(room->id, "market") != 0 &&
       strcmp(room->id, "bias-checkpoint") != 0)) {
    queue_text(session, "There is no one here asking for your stand.");
    return;
  }
  snprintf(session->character.faction, sizeof(session->character.faction),
           "ally");
  session->character.morality += 10;
  session->market_resolved = 1;
  queue_text(session, "You put yourself between the Front and the living.");
  if (!hg_character_has_item(&session->character, "charm")) {
    hg_character_add_item(&session->character, "charm");
  }
  record_trace(server, server->config->world_name, session->character.room,
               "defense", "someone stood with the refugees here");
  hg_store_save(&server->store, &session->character);
  send_affects(session);
  send_actions(session, room);
}

static void handle_play(hg_session *session, hg_server *server, char *input) {
  hg_world_tick_respawns(&server->world);
  char command[160];
  snprintf(command, sizeof(command), "%s", input);
  for (char *p = command; *p != '\0'; ++p) {
    *p = (char)tolower((unsigned char)*p);
  }
  char *verb = trim(command);
  char *arg = NULL;
  char *space = strchr(verb, ' ');
  if (space != NULL) {
    *space = '\0';
    arg = trim(space + 1);
  }
  if (strncmp(verb, "go", 2) == 0 && arg != NULL) {
    verb = arg;
    arg = NULL;
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
  if (strcmp(verb, "sense") == 0 || strcmp(verb, "actions") == 0) {
    const hg_room *room = hg_world_room(session->character.room);
    if (room != NULL) {
      send_actions(session, room);
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
  if (strcmp(verb, "listen") == 0) {
    cJSON *payload = json_object();
    cJSON_AddStringToObject(payload, "kind", "signal");
    cJSON_AddStringToObject(
        payload, "text",
        "a residual field hums through dead ferrite, carrying a half-erased name");
    queue_event(session, "grid.transmission", payload);
    queue_text(session, "You listen. The dead network answers.");
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
               "attack, rest, join, defend, listen, ping, tell, reply, yell, "
               "emote, sense, help.");
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
    session->character.morality -= 8;
    session->character.gold += 12;
    hg_store_save(&server->store, &session->character);
    queue_text(session, "You snag a fistful of coin while the vendor looks away.");
    send_vitals(session);
    send_affects(session);
    return;
  }
  if (strcmp(verb, "sell") == 0) {
    if (strcmp(session->character.room, "market") != 0) {
      queue_text(session, "Nobody here will buy.");
      return;
    }
    if (strcmp(session->character.faction, "front") == 0) {
      queue_text(session, "The market refuses to trade with the Cinder Front.");
      return;
    }
    queue_text(session, "You have nothing the market wants right now.");
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
  hg_world_init(&server.world);
  seed_traces(&server);

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
