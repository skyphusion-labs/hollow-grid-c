#include "hg_server.h"

#include "hg_event.h"
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
  struct lws *wsi;
  hg_session_state state;
  hg_character character;
  hg_message *out_head;
  hg_message *out_tail;
  char input[512];
  size_t input_length;
} hg_session;

typedef struct {
  const hg_server_config *config;
  struct lws_context *context;
  hg_store store;
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

static void send_room_info(hg_session *session, const hg_room *room) {
  cJSON *payload = json_object();
  cJSON_AddStringToObject(payload, "id", room->id);
  cJSON_AddStringToObject(payload, "name", room->name);

  cJSON *exits = cJSON_AddArrayToObject(payload, "exits");
  for (size_t i = 0; i < room->exit_count; ++i) {
    cJSON_AddItemToArray(exits,
                        cJSON_CreateString(room->exits[i].direction));
  }

  cJSON *mobs = cJSON_AddArrayToObject(payload, "mobs");
  for (size_t i = 0; i < room->mob_count; ++i) {
    cJSON *mob = cJSON_CreateObject();
    cJSON_AddStringToObject(mob, "id", room->mobs[i].id);
    cJSON_AddStringToObject(mob, "name", room->mobs[i].name);
    cJSON_AddNumberToObject(mob, "hp", room->mobs[i].hp);
    cJSON_AddNumberToObject(mob, "maxHp", room->mobs[i].max_hp);
    cJSON_AddItemToArray(mobs, mob);
  }
  cJSON_AddArrayToObject(payload, "items");
  cJSON_AddArrayToObject(payload, "players");
  queue_event(session, "room.info", payload);
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
  cJSON_AddBoolToObject(payload, "inCombat", 0);
  cJSON_AddBoolToObject(payload, "poisoned", 0);
  cJSON_AddStringToObject(payload, "position", "standing");
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

static void send_actions(hg_session *session, const hg_room *room) {
  cJSON *payload = json_object();
  cJSON *actions = cJSON_AddArrayToObject(payload, "actions");
  for (size_t i = 0; i < room->action_count; ++i) {
    cJSON *action = cJSON_CreateObject();
    cJSON_AddStringToObject(action, "verb", room->actions[i].verb);
    cJSON_AddStringToObject(action, "label", room->actions[i].label);
    cJSON_AddStringToObject(action, "kind", room->actions[i].kind);
    if (room->actions[i].valence != NULL) {
      cJSON_AddStringToObject(action, "valence", room->actions[i].valence);
    }
    cJSON_AddItemToArray(actions, action);
  }
  queue_event(session, "room.actions", payload);
}

static void send_scene(hg_session *session, const hg_server *server) {
  const hg_room *room = hg_world_room(session->character.room);
  if (room == NULL) {
    snprintf(session->character.room, sizeof(session->character.room), "nexus");
    room = hg_world_start();
  }

  queue_text(session, "\033[1;38;5;208m%s\033[0m", room->name);
  queue_text(session, "%s", room->description);
  if (room->exit_count > 0) {
    char exit_line[160] = "Exits:";
    size_t used = strlen(exit_line);
    for (size_t i = 0; i < room->exit_count; ++i) {
      int written =
          snprintf(exit_line + used, sizeof(exit_line) - used, " %s",
                   room->exits[i].direction);
      if (written < 0 || (size_t)written >= sizeof(exit_line) - used) {
        break;
      }
      used += (size_t)written;
    }
    queue_text(session, "%s", exit_line);
  }
  send_room_info(session, room);
  send_vitals(session);
  send_affects(session);
  send_actions(session, room);
  (void)server;
}

static void send_creation_menu(hg_session *session) {
  queue_text(session, "Choose what the wastes made of you:");
  queue_text(session,
             "  1) Human      2) Elf       3) Revenant   4) Ghoul");
  queue_text(session,
             "  5) Chromed    6) Dustkin   7) Vatborn");
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

static void finish_login(hg_session *session, hg_server *server,
                         int resumed) {
  session->state = HG_PLAYING;
  if (hg_world_room(session->character.room) == NULL) {
    snprintf(session->character.room, sizeof(session->character.room), "nexus");
  }
  queue_text(session, resumed ? "The Grid finds your old charge, %s."
                              : "The field takes your measure, %s.",
             session->character.name);
  send_world_state(session, server);
  send_scene(session, server);
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

static void handle_play(hg_session *session, hg_server *server,
                        const char *input) {
  char command[128];
  snprintf(command, sizeof(command), "%s", input);
  for (char *p = command; *p != '\0'; ++p) {
    *p = (char)tolower((unsigned char)*p);
  }
  char *verb = trim(command);
  if (strncmp(verb, "go ", 3) == 0) {
    verb = trim(verb + 3);
  }

  if (strcmp(verb, "look") == 0 || strcmp(verb, "l") == 0) {
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

  const hg_room *room = hg_world_room(session->character.room);
  const hg_room *destination = hg_world_move(room, verb);
  if (destination != NULL) {
    snprintf(session->character.room, sizeof(session->character.room), "%s",
             destination->id);
    hg_store_save(&server->store, &session->character);
    send_scene(session, server);
    return;
  }

  queue_text(session, "Nothing answers '%s'. Type look to read the room.", input);
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
    int copied =
        lws_hdr_copy(wsi, path, sizeof(path), WSI_TOKEN_GET_URI);
    return copied > 0 && strcmp(path, "/ws") == 0 ? 0 : 1;
  }
  case LWS_CALLBACK_HTTP:
    return handle_http(wsi, server, (const char *)in);
  case LWS_CALLBACK_ESTABLISHED:
    memset(session, 0, sizeof(*session));
    session->wsi = wsi;
    session->state = HG_WAIT_NAME;
    queue_text(session, "\033[1;38;5;208mFERRITE WASTES\033[0m");
    queue_text(session,
               "The field is gone. What it changed remains.");
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
  case LWS_CALLBACK_SERVER_WRITEABLE:
    if (session->out_head != NULL) {
      hg_message *message = session->out_head;
      size_t length = strlen(message->text);
      unsigned char *buffer = malloc(LWS_PRE + length);
      if (buffer == NULL) {
        return -1;
      }
      memcpy(buffer + LWS_PRE, message->text, length);
      int written =
          lws_write(wsi, buffer + LWS_PRE, length, LWS_WRITE_TEXT);
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

  fprintf(stdout, "%s listening on %s:%d\n", config->world_name,
          config->host, config->port);
  stop_requested = 0;
  while (!stop_requested) {
    if (lws_service(server.context, 100) < 0) {
      break;
    }
  }
  lws_context_destroy(server.context);
  return 0;
}
