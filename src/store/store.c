#include "hg_store.h"

#include <cjson/cJSON.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int make_dir(const char *path) {
  if (mkdir(path, 0755) == 0 || errno == EEXIST) {
    return 0;
  }
  return -1;
}

static int character_path(const hg_store *store, const char *name, char *out,
                          size_t out_size) {
  char key[33];
  size_t length = strlen(name);
  if (length == 0 || length >= sizeof(key)) {
    return -1;
  }
  for (size_t i = 0; i < length; ++i) {
    key[i] = (char)tolower((unsigned char)name[i]);
  }
  key[length] = '\0';
  int written = snprintf(out, out_size, "%s/%s.json", store->root, key);
  return written > 0 && (size_t)written < out_size ? 0 : -1;
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

int hg_store_init(hg_store *store, const char *data_dir) {
  if (store == NULL || data_dir == NULL || data_dir[0] == '\0') {
    return -1;
  }
  if (make_dir(data_dir) != 0) {
    return -1;
  }
  int written =
      snprintf(store->root, sizeof(store->root), "%s/characters", data_dir);
  if (written <= 0 || (size_t)written >= sizeof(store->root)) {
    return -1;
  }
  return make_dir(store->root);
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
  character->hp = 30;
  character->max_hp = 30;
  character->level = 1;
}

int hg_store_load(const hg_store *store, const char *name, hg_character *out) {
  char path[640];
  if (character_path(store, name, path, sizeof(path)) != 0) {
    return -1;
  }
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return 0;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return -1;
  }
  long size = ftell(file);
  if (size < 2 || size > 65536 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return -1;
  }
  char *text = malloc((size_t)size + 1);
  if (text == NULL) {
    fclose(file);
    return -1;
  }
  size_t read_count = fread(text, 1, (size_t)size, file);
  fclose(file);
  text[read_count] = '\0';

  cJSON *root = cJSON_Parse(text);
  free(text);
  if (root == NULL) {
    return -1;
  }

  hg_character_new(out, name);
  copy_json_string(root, "name", out->name, sizeof(out->name));
  copy_json_string(root, "race", out->race, sizeof(out->race));
  copy_json_string(root, "room", out->room, sizeof(out->room));
  copy_json_string(root, "faction", out->faction, sizeof(out->faction));
  out->hp = json_int(root, "hp", out->hp);
  out->max_hp = json_int(root, "maxHp", out->max_hp);
  out->level = json_int(root, "level", out->level);
  out->xp = json_int(root, "xp", 0);
  out->gold = json_int(root, "gold", 0);
  out->morality = json_int(root, "morality", 0);
  out->addiction = json_int(root, "addiction", 0);
  out->ashsworn = json_int(root, "ashsworn", 0);
  cJSON_Delete(root);
  return 1;
}

int hg_store_save(const hg_store *store, const hg_character *character) {
  char path[640];
  char temp_path[656];
  if (character_path(store, character->name, path, sizeof(path)) != 0) {
    return -1;
  }
  int written =
      snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
  if (written <= 0 || (size_t)written >= sizeof(temp_path)) {
    return -1;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    return -1;
  }
  cJSON_AddStringToObject(root, "name", character->name);
  cJSON_AddStringToObject(root, "race", character->race);
  cJSON_AddStringToObject(root, "room", character->room);
  cJSON_AddStringToObject(root, "faction", character->faction);
  cJSON_AddNumberToObject(root, "hp", character->hp);
  cJSON_AddNumberToObject(root, "maxHp", character->max_hp);
  cJSON_AddNumberToObject(root, "level", character->level);
  cJSON_AddNumberToObject(root, "xp", character->xp);
  cJSON_AddNumberToObject(root, "gold", character->gold);
  cJSON_AddNumberToObject(root, "morality", character->morality);
  cJSON_AddNumberToObject(root, "addiction", character->addiction);
  cJSON_AddBoolToObject(root, "ashsworn", character->ashsworn);
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json == NULL) {
    return -1;
  }

  FILE *file = fopen(temp_path, "wb");
  if (file == NULL) {
    free(json);
    return -1;
  }
  size_t length = strlen(json);
  int ok = fwrite(json, 1, length, file) == length && fputc('\n', file) != EOF;
  if (fclose(file) != 0) {
    ok = 0;
  }
  free(json);
  if (!ok) {
    remove(temp_path);
    return -1;
  }
  if (rename(temp_path, path) != 0) {
    remove(temp_path);
    return -1;
  }
  return 0;
}
