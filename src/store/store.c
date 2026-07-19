#define _POSIX_C_SOURCE 200809L

#include "hg_store.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Records are stored as a single-file SQLite key-value table: the key is the
   lowercased character name and the value is the same JSON document the
   file-backed store used. Keeping the JSON schema unchanged means smoke
   behaviour is identical and any pre-existing per-character files import
   cleanly on first launch (see migrate_legacy_json). */

static int make_dir(const char *path) {
  if (mkdir(path, 0755) == 0 || errno == EEXIST) {
    return 0;
  }
  return -1;
}

/* Lowercase the name into a bounded key. Returns 0 on success. */
static int name_key(const char *name, char *out, size_t out_size) {
  if (name == NULL) {
    return -1;
  }
  size_t length = strlen(name);
  if (length == 0 || length >= out_size) {
    return -1;
  }
  for (size_t i = 0; i < length; ++i) {
    out[i] = (char)tolower((unsigned char)name[i]);
  }
  out[length] = '\0';
  return 0;
}

static void copy_json_string(cJSON *root, const char *key, char *out,
                             size_t out_size) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsString(item) && item->valuestring != NULL) {
    snprintf(out, out_size, "%s", item->valuestring);
  }
}

static int json_int(cJSON *root, const char *key, int fallback) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  return cJSON_IsNumber(item) ? item->valueint : fallback;
}

/* Flags are written with cJSON bools; accept either bool or a 0/1 number so
   records survive a round-trip and tolerate hand-edited data. */
static int json_flag(cJSON *root, const char *key) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsBool(item)) {
    return cJSON_IsTrue(item) ? 1 : 0;
  }
  return cJSON_IsNumber(item) && item->valueint != 0 ? 1 : 0;
}

/* Parse a stored JSON document into a character. Returns 1 on success, -1 on
   malformed input. The name argument seeds defaults before overrides apply. */
static int character_from_json(const char *text, const char *name,
                               hg_character *out) {
  cJSON *root = cJSON_Parse(text);
  if (root == NULL) {
    return -1;
  }
  hg_character_new(out, name);
  out->inventory_count = 0;
  memset(out->inventory, 0, sizeof(out->inventory));
  copy_json_string(root, "name", out->name, sizeof(out->name));
  copy_json_string(root, "race", out->race, sizeof(out->race));
  copy_json_string(root, "room", out->room, sizeof(out->room));
  copy_json_string(root, "faction", out->faction, sizeof(out->faction));
  copy_json_string(root, "position", out->position, sizeof(out->position));
  copy_json_string(root, "title", out->title, sizeof(out->title));
  copy_json_string(root, "weapon", out->weapon, sizeof(out->weapon));
  out->hp = json_int(root, "hp", out->hp);
  out->max_hp = json_int(root, "maxHp", out->max_hp);
  out->level = json_int(root, "level", out->level);
  out->xp = json_int(root, "xp", 0);
  out->gold = json_int(root, "gold", 0);
  out->morality = json_int(root, "morality", 0);
  out->addiction = json_int(root, "addiction", 0);
  out->ashsworn = json_flag(root, "ashsworn");
  out->strayed = json_flag(root, "strayed");
  out->redeemed = json_flag(root, "redeemed");

  cJSON *inventory = cJSON_GetObjectItemCaseSensitive(root, "inventory");
  if (cJSON_IsArray(inventory)) {
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, inventory) {
      if (cJSON_IsString(entry) && entry->valuestring != NULL &&
          out->inventory_count < HG_MAX_INVENTORY) {
        snprintf(out->inventory[out->inventory_count],
                 sizeof(out->inventory[0]), "%s", entry->valuestring);
        out->inventory_count++;
      }
    }
  }
  if (out->inventory_count == 0) {
    snprintf(out->inventory[0], sizeof(out->inventory[0]), "shiv");
    out->inventory_count = 1;
  }
  if (out->position[0] == '\0') {
    snprintf(out->position, sizeof(out->position), "standing");
  }
  cJSON_Delete(root);
  return 1;
}

/* Serialize a character to a JSON document. Caller frees the result. */
static char *character_to_json(const hg_character *character) {
  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    return NULL;
  }
  cJSON_AddStringToObject(root, "name", character->name);
  cJSON_AddStringToObject(root, "race", character->race);
  cJSON_AddStringToObject(root, "room", character->room);
  cJSON_AddStringToObject(root, "faction", character->faction);
  cJSON_AddStringToObject(root, "position", character->position);
  cJSON_AddStringToObject(root, "title", character->title);
  cJSON_AddStringToObject(root, "weapon", character->weapon);
  cJSON_AddNumberToObject(root, "hp", character->hp);
  cJSON_AddNumberToObject(root, "maxHp", character->max_hp);
  cJSON_AddNumberToObject(root, "level", character->level);
  cJSON_AddNumberToObject(root, "xp", character->xp);
  cJSON_AddNumberToObject(root, "gold", character->gold);
  cJSON_AddNumberToObject(root, "morality", character->morality);
  cJSON_AddNumberToObject(root, "addiction", character->addiction);
  cJSON_AddBoolToObject(root, "ashsworn", character->ashsworn);
  cJSON_AddBoolToObject(root, "strayed", character->strayed);
  cJSON_AddBoolToObject(root, "redeemed", character->redeemed);
  cJSON *inventory = cJSON_AddArrayToObject(root, "inventory");
  for (size_t i = 0; i < character->inventory_count; ++i) {
    cJSON_AddItemToArray(inventory,
                         cJSON_CreateString(character->inventory[i]));
  }
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return json;
}

/* INSERT OR IGNORE one legacy record so an import never clobbers a value that
   is already newer in the database. */
static void import_record(sqlite3 *db, const char *key, const char *json) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(
          db, "INSERT OR IGNORE INTO characters(key, data) VALUES(?, ?)", -1,
          &stmt, NULL) != SQLITE_OK) {
    return;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, json, -1, SQLITE_STATIC);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

/* Best-effort one-time import of any pre-SQLite per-character JSON records
   under <data_dir>/characters. Failures are non-fatal: a fresh deploy simply
   has nothing to import. */
static void migrate_legacy_json(sqlite3 *db, const char *data_dir) {
  char dir_path[512];
  int written =
      snprintf(dir_path, sizeof(dir_path), "%s/characters", data_dir);
  if (written <= 0 || (size_t)written >= sizeof(dir_path)) {
    return;
  }
  DIR *dir = opendir(dir_path);
  if (dir == NULL) {
    return;
  }
  struct dirent *entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    const char *dot = strrchr(entry->d_name, '.');
    if (dot == NULL || strcmp(dot, ".json") != 0) {
      continue;
    }
    size_t stem_len = (size_t)(dot - entry->d_name);
    char key[64];
    if (stem_len == 0 || stem_len >= sizeof(key)) {
      continue;
    }
    memcpy(key, entry->d_name, stem_len);
    key[stem_len] = '\0';

    char file_path[1024];
    written = snprintf(file_path, sizeof(file_path), "%s/%s", dir_path,
                       entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(file_path)) {
      continue;
    }
    FILE *file = fopen(file_path, "rb");
    if (file == NULL) {
      continue;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
      fclose(file);
      continue;
    }
    long size = ftell(file);
    if (size < 2 || size > 65536 || fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      continue;
    }
    char *text = malloc((size_t)size + 1);
    if (text == NULL) {
      fclose(file);
      continue;
    }
    size_t read_count = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[read_count] = '\0';
    /* Only import records that parse; skip garbage rather than abort. */
    cJSON *probe = cJSON_Parse(text);
    if (probe != NULL) {
      cJSON_Delete(probe);
      import_record(db, key, text);
    }
    free(text);
  }
  closedir(dir);
}

int hg_store_init(hg_store *store, const char *data_dir) {
  if (store == NULL || data_dir == NULL || data_dir[0] == '\0') {
    return -1;
  }
  store->db = NULL;
  if (make_dir(data_dir) != 0) {
    return -1;
  }
  int written =
      snprintf(store->path, sizeof(store->path), "%s/characters.db", data_dir);
  if (written <= 0 || (size_t)written >= sizeof(store->path)) {
    return -1;
  }

  sqlite3 *db = NULL;
  if (sqlite3_open(store->path, &db) != SQLITE_OK) {
    sqlite3_close(db);
    return -1;
  }
  /* WAL + a short busy timeout keep single-writer commits durable and tolerate
     the occasional overlapping read without spurious SQLITE_BUSY. */
  sqlite3_busy_timeout(db, 2000);
  if (sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL) !=
          SQLITE_OK ||
      sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL) !=
          SQLITE_OK ||
      sqlite3_exec(db,
                   "CREATE TABLE IF NOT EXISTS characters ("
                   "key TEXT PRIMARY KEY, "
                   "name TEXT, "
                   "data TEXT NOT NULL);",
                   NULL, NULL, NULL) != SQLITE_OK) {
    sqlite3_close(db);
    return -1;
  }

  migrate_legacy_json(db, data_dir);
  store->db = db;
  return 0;
}

void hg_store_close(hg_store *store) {
  if (store == NULL || store->db == NULL) {
    return;
  }
  sqlite3_close(store->db);
  store->db = NULL;
}

int hg_store_valid_name(const char *name) {
  if (name == NULL) {
    return 0;
  }
  size_t length = strlen(name);
  if (length < 2 || length > 32) {
    return 0;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char ch = (unsigned char)name[i];
    if (!isalnum(ch) && ch != '_' && ch != '-') {
      return 0;
    }
  }
  return 1;
}

void hg_character_new(hg_character *character, const char *name) {
  memset(character, 0, sizeof(*character));
  snprintf(character->name, sizeof(character->name), "%s", name);
  snprintf(character->room, sizeof(character->room), "nexus");
  snprintf(character->faction, sizeof(character->faction), "none");
  snprintf(character->position, sizeof(character->position), "standing");
  snprintf(character->inventory[0], sizeof(character->inventory[0]), "shiv");
  character->inventory_count = 1;
  character->hp = 30;
  character->max_hp = 30;
  character->level = 1;
  character->gold = 20;
}

int hg_character_has_item(const hg_character *character, const char *item_id) {
  if (character == NULL || item_id == NULL) {
    return 0;
  }
  for (size_t i = 0; i < character->inventory_count; ++i) {
    if (strcmp(character->inventory[i], item_id) == 0) {
      return 1;
    }
  }
  return 0;
}

int hg_character_add_item(hg_character *character, const char *item_id) {
  if (character == NULL || item_id == NULL ||
      character->inventory_count >= HG_MAX_INVENTORY) {
    return -1;
  }
  snprintf(character->inventory[character->inventory_count],
           sizeof(character->inventory[0]), "%s", item_id);
  character->inventory_count++;
  return 0;
}

int hg_character_remove_item(hg_character *character, const char *item_id) {
  if (character == NULL || item_id == NULL) {
    return -1;
  }
  for (size_t i = 0; i < character->inventory_count; ++i) {
    if (strcmp(character->inventory[i], item_id) == 0) {
      for (size_t j = i + 1; j < character->inventory_count; ++j) {
        memmove(character->inventory[j - 1], character->inventory[j],
                sizeof(character->inventory[0]));
      }
      character->inventory_count--;
      memset(character->inventory[character->inventory_count], 0,
             sizeof(character->inventory[0]));
      return 0;
    }
  }
  return -1;
}

int hg_store_load(const hg_store *store, const char *name, hg_character *out) {
  if (store == NULL || store->db == NULL) {
    return -1;
  }
  char key[64];
  if (name_key(name, key, sizeof(key)) != 0) {
    return -1;
  }
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(store->db,
                         "SELECT data FROM characters WHERE key = ?", -1, &stmt,
                         NULL) != SQLITE_OK) {
    return -1;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return 0; /* No such record: not an error. */
  }
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return -1;
  }
  const char *text = (const char *)sqlite3_column_text(stmt, 0);
  int result = text != NULL ? character_from_json(text, name, out) : -1;
  sqlite3_finalize(stmt);
  return result;
}

int hg_store_save(const hg_store *store, const hg_character *character) {
  if (store == NULL || store->db == NULL || character == NULL) {
    return -1;
  }
  char key[64];
  if (name_key(character->name, key, sizeof(key)) != 0) {
    return -1;
  }
  char *json = character_to_json(character);
  if (json == NULL) {
    return -1;
  }
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(store->db,
                         "INSERT INTO characters(key, name, data) "
                         "VALUES(?, ?, ?) "
                         "ON CONFLICT(key) DO UPDATE SET "
                         "name = excluded.name, data = excluded.data",
                         -1, &stmt, NULL) != SQLITE_OK) {
    free(json);
    return -1;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, character->name, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, json, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  free(json);
  return rc == SQLITE_DONE ? 0 : -1;
}
