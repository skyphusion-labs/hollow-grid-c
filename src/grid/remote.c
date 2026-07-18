#include "hg_grid.h"

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

struct hg_grid {
  char *url;
  char *token;
  CURL *curl;
  int curl_global_owned;
};

struct memory_buf {
  char *data;
  size_t size;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  struct memory_buf *buf = userdata;
  size_t add = size * nmemb;
  char *next = realloc(buf->data, buf->size + add + 1);
  if (next == NULL) {
    return 0;
  }
  buf->data = next;
  memcpy(buf->data + buf->size, ptr, add);
  buf->size += add;
  buf->data[buf->size] = '\0';
  return add;
}

static long long now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + (long long)tv.tv_usec / 1000;
}

static char *dup_str(const char *s) {
  if (s == NULL) {
    s = "";
  }
  size_t n = strlen(s);
  char *out = malloc(n + 1);
  if (out == NULL) {
    return NULL;
  }
  memcpy(out, s, n + 1);
  return out;
}

hg_grid *hg_grid_open(const char *hub_url, const char *token) {
  if (hub_url == NULL || hub_url[0] == '\0') {
    return NULL;
  }
  hg_grid *grid = calloc(1, sizeof(*grid));
  if (grid == NULL) {
    return NULL;
  }
  grid->url = dup_str(hub_url);
  grid->token = dup_str(token);
  if (grid->url == NULL || grid->token == NULL) {
    hg_grid_close(grid);
    return NULL;
  }
  size_t len = strlen(grid->url);
  while (len > 0 && grid->url[len - 1] == '/') {
    grid->url[--len] = '\0';
  }
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
    hg_grid_close(grid);
    return NULL;
  }
  grid->curl_global_owned = 1;
  grid->curl = curl_easy_init();
  if (grid->curl == NULL) {
    hg_grid_close(grid);
    return NULL;
  }
  return grid;
}

void hg_grid_close(hg_grid *grid) {
  if (grid == NULL) {
    return;
  }
  if (grid->curl != NULL) {
    curl_easy_cleanup(grid->curl);
  }
  if (grid->curl_global_owned) {
    curl_global_cleanup();
  }
  free(grid->url);
  free(grid->token);
  free(grid);
}

/* 0 = ok (result may be NULL for JSON null). -1 = transport/RPC failure. */
static int rpc_call(hg_grid *grid, const char *method, cJSON *params,
                    cJSON **out_result) {
  if (out_result != NULL) {
    *out_result = NULL;
  }
  if (grid == NULL || method == NULL) {
    cJSON_Delete(params);
    return -1;
  }
  cJSON *req = cJSON_CreateObject();
  if (req == NULL) {
    cJSON_Delete(params);
    return -1;
  }
  cJSON_AddStringToObject(req, "method", method);
  if (params == NULL) {
    params = cJSON_CreateArray();
  }
  cJSON_AddItemToObject(req, "params", params);
  char *body = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);
  if (body == NULL) {
    return -1;
  }

  struct memory_buf buf = {0};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "User-Agent: hollow-grid-c/0.1.0");
  char auth[640];
  if (grid->token[0] != '\0') {
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", grid->token);
    headers = curl_slist_append(headers, auth);
  }

  curl_easy_reset(grid->curl);
  curl_easy_setopt(grid->curl, CURLOPT_URL, grid->url);
  curl_easy_setopt(grid->curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(grid->curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(grid->curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(grid->curl, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt(grid->curl, CURLOPT_TIMEOUT, 2L);
  curl_easy_setopt(grid->curl, CURLOPT_CONNECTTIMEOUT, 2L);

  CURLcode rc = curl_easy_perform(grid->curl);
  curl_slist_free_all(headers);
  free(body);
  if (rc != CURLE_OK) {
    free(buf.data);
    return -1;
  }

  cJSON *wrap = cJSON_Parse(buf.data != NULL ? buf.data : "");
  free(buf.data);
  if (wrap == NULL) {
    return -1;
  }
  cJSON *ok = cJSON_GetObjectItemCaseSensitive(wrap, "ok");
  if (!cJSON_IsTrue(ok)) {
    cJSON_Delete(wrap);
    return -1;
  }
  cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(wrap, "result");
  cJSON_Delete(wrap);
  if (out_result != NULL) {
    *out_result = result;
  } else {
    cJSON_Delete(result);
  }
  return 0;
}

static const char *json_str(const cJSON *obj, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring
                                                           : "";
}

static int json_int(const cJSON *obj, const char *key, int fallback) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static long long json_ll(const cJSON *obj, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsNumber(item)) {
    return 0;
  }
  return (long long)item->valuedouble;
}

int hg_grid_ping(hg_grid *grid, long *latency_ms) {
  long long start = now_ms();
  int tide = 0;
  int rc = hg_grid_tide(grid, &tide);
  if (latency_ms != NULL) {
    *latency_ms = (long)(now_ms() - start);
  }
  return rc;
}

int hg_grid_register(hg_grid *grid, const char *world, const char *url) {
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateString(world != NULL ? world : ""));
  cJSON_AddItemToArray(params, cJSON_CreateString(url != NULL ? url : ""));
  return rpc_call(grid, "register", params, NULL);
}

int hg_grid_tide(hg_grid *grid, int *out_tide) {
  cJSON *result = NULL;
  if (rpc_call(grid, "tide", cJSON_CreateArray(), &result) != 0) {
    return -1;
  }
  if (!cJSON_IsNumber(result)) {
    cJSON_Delete(result);
    return -1;
  }
  if (out_tide != NULL) {
    *out_tide = result->valueint;
  }
  cJSON_Delete(result);
  return 0;
}

int hg_grid_shift_tide(hg_grid *grid, int delta, int *out_tide) {
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateNumber(delta));
  cJSON *result = NULL;
  if (rpc_call(grid, "shiftTide", params, &result) != 0) {
    return -1;
  }
  if (!cJSON_IsNumber(result)) {
    cJSON_Delete(result);
    return -1;
  }
  if (out_tide != NULL) {
    *out_tide = result->valueint;
  }
  cJSON_Delete(result);
  return 0;
}

int hg_grid_gridcast(hg_grid *grid, const char *world, const char *sender,
                     const char *text) {
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateString(world != NULL ? world : ""));
  cJSON_AddItemToArray(params, cJSON_CreateString(sender != NULL ? sender : ""));
  cJSON_AddItemToArray(params, cJSON_CreateString(text != NULL ? text : ""));
  return rpc_call(grid, "gridcast", params, NULL);
}

int hg_grid_casts_since(hg_grid *grid, int since_id, int limit,
                        hg_grid_cast *out, size_t cap, size_t *out_count) {
  if (out_count != NULL) {
    *out_count = 0;
  }
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateNumber(since_id));
  cJSON_AddItemToArray(params, cJSON_CreateNumber(limit));
  cJSON *result = NULL;
  if (rpc_call(grid, "castsSince", params, &result) != 0) {
    return -1;
  }
  if (!cJSON_IsArray(result)) {
    cJSON_Delete(result);
    return -1;
  }
  size_t n = 0;
  const cJSON *row = NULL;
  cJSON_ArrayForEach(row, result) {
    if (n >= cap) {
      break;
    }
    out[n].id = json_int(row, "id", 0);
    snprintf(out[n].world, sizeof(out[n].world), "%s", json_str(row, "world"));
    snprintf(out[n].sender, sizeof(out[n].sender), "%s",
             json_str(row, "sender"));
    snprintf(out[n].text, sizeof(out[n].text), "%s", json_str(row, "text"));
    n++;
  }
  cJSON_Delete(result);
  if (out_count != NULL) {
    *out_count = n;
  }
  return 0;
}

int hg_grid_list_worlds(hg_grid *grid, hg_grid_world *out, size_t cap,
                        size_t *out_count) {
  if (out_count != NULL) {
    *out_count = 0;
  }
  cJSON *result = NULL;
  if (rpc_call(grid, "listWorlds", cJSON_CreateArray(), &result) != 0) {
    return -1;
  }
  if (!cJSON_IsArray(result)) {
    cJSON_Delete(result);
    return -1;
  }
  size_t n = 0;
  const cJSON *row = NULL;
  cJSON_ArrayForEach(row, result) {
    if (n >= cap) {
      break;
    }
    snprintf(out[n].id, sizeof(out[n].id), "%s", json_str(row, "id"));
    snprintf(out[n].url, sizeof(out[n].url), "%s", json_str(row, "url"));
    out[n].last_seen_ms = json_ll(row, "last_seen");
    n++;
  }
  cJSON_Delete(result);
  if (out_count != NULL) {
    *out_count = n;
  }
  return 0;
}

int hg_grid_report_presence(hg_grid *grid, const char *world,
                            const hg_grid_presence *entries, size_t count,
                            long long at_ms) {
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateString(world != NULL ? world : ""));
  cJSON *rows = cJSON_CreateArray();
  for (size_t i = 0; i < count; ++i) {
    cJSON *row = cJSON_CreateObject();
    cJSON_AddStringToObject(row, "name", entries[i].name);
    cJSON_AddStringToObject(row, "regard", entries[i].regard);
    cJSON_AddStringToObject(row, "title", entries[i].title);
    cJSON_AddItemToArray(rows, row);
  }
  cJSON_AddItemToArray(params, rows);
  cJSON_AddItemToArray(params, cJSON_CreateNumber((double)at_ms));
  return rpc_call(grid, "reportPresence", params, NULL);
}

int hg_grid_fetch_presence(hg_grid *grid, long long max_age_ms,
                           hg_grid_presence *out, size_t cap,
                           size_t *out_count) {
  if (out_count != NULL) {
    *out_count = 0;
  }
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateNumber((double)max_age_ms));
  cJSON *result = NULL;
  if (rpc_call(grid, "presence", params, &result) != 0) {
    return -1;
  }
  if (!cJSON_IsArray(result)) {
    cJSON_Delete(result);
    return -1;
  }
  size_t n = 0;
  const cJSON *row = NULL;
  cJSON_ArrayForEach(row, result) {
    if (n >= cap) {
      break;
    }
    snprintf(out[n].world, sizeof(out[n].world), "%s", json_str(row, "world"));
    snprintf(out[n].name, sizeof(out[n].name), "%s", json_str(row, "name"));
    snprintf(out[n].regard, sizeof(out[n].regard), "%s",
             json_str(row, "regard"));
    snprintf(out[n].title, sizeof(out[n].title), "%s", json_str(row, "title"));
    out[n].at_ms = json_ll(row, "at");
    n++;
  }
  cJSON_Delete(result);
  if (out_count != NULL) {
    *out_count = n;
  }
  return 0;
}

static void sheet_from_json(const cJSON *obj, hg_char_sheet *out) {
  memset(out, 0, sizeof(*out));
  out->level = json_int(obj, "level", 1);
  out->xp = json_int(obj, "xp", 0);
  out->gold = json_int(obj, "gold", 0);
  out->morality = json_int(obj, "morality", 0);
  snprintf(out->faction, sizeof(out->faction), "%s", json_str(obj, "faction"));
  snprintf(out->title, sizeof(out->title), "%s", json_str(obj, "title"));
  snprintf(out->race, sizeof(out->race), "%s", json_str(obj, "race"));
  const cJSON *ash = cJSON_GetObjectItemCaseSensitive(obj, "ashsworn");
  out->ashsworn = cJSON_IsTrue(ash);
}

int hg_grid_load_character(hg_grid *grid, const char *name, hg_char_sheet *out,
                           int *found) {
  if (found != NULL) {
    *found = 0;
  }
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateString(name != NULL ? name : ""));
  cJSON *result = NULL;
  if (rpc_call(grid, "loadCharacter", params, &result) != 0) {
    return -1;
  }
  if (!cJSON_IsObject(result)) {
    cJSON_Delete(result);
    return -1;
  }
  sheet_from_json(result, out);
  if (found != NULL) {
    *found = out->race[0] != '\0' || out->level > 1 || out->xp > 0 ||
             out->faction[0] != '\0' || out->morality != 0;
  }
  cJSON_Delete(result);
  return 0;
}

int hg_grid_commit_character(hg_grid *grid, const char *name,
                             const hg_char_sheet *sheet) {
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateString(name != NULL ? name : ""));
  cJSON *obj = cJSON_CreateObject();
  cJSON_AddNumberToObject(obj, "level", sheet->level);
  cJSON_AddNumberToObject(obj, "xp", sheet->xp);
  cJSON_AddNumberToObject(obj, "gold", sheet->gold);
  cJSON_AddStringToObject(obj, "faction", sheet->faction);
  cJSON_AddNumberToObject(obj, "morality", sheet->morality);
  cJSON_AddStringToObject(obj, "title", sheet->title);
  cJSON_AddStringToObject(obj, "race", sheet->race);
  cJSON_AddBoolToObject(obj, "ashsworn", sheet->ashsworn);
  cJSON_AddItemToArray(params, obj);
  return rpc_call(grid, "commitCharacter", params, NULL);
}

int hg_grid_record_fallen(hg_grid *grid, const char *world, const char *name,
                          const char *room, long long at_ms) {
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateString(world != NULL ? world : ""));
  cJSON_AddItemToArray(params, cJSON_CreateString(name != NULL ? name : ""));
  cJSON_AddItemToArray(params, cJSON_CreateString(room != NULL ? room : ""));
  cJSON_AddItemToArray(params, cJSON_CreateNumber((double)at_ms));
  return rpc_call(grid, "recordFallen", params, NULL);
}

int hg_grid_recent_fallen(hg_grid *grid, int limit, hg_grid_fallen *out,
                          size_t cap, size_t *out_count) {
  if (out_count != NULL) {
    *out_count = 0;
  }
  cJSON *params = cJSON_CreateArray();
  cJSON_AddItemToArray(params, cJSON_CreateNumber(limit));
  cJSON *result = NULL;
  if (rpc_call(grid, "recentFallen", params, &result) != 0) {
    return -1;
  }
  if (!cJSON_IsArray(result)) {
    cJSON_Delete(result);
    return -1;
  }
  size_t n = 0;
  const cJSON *row = NULL;
  cJSON_ArrayForEach(row, result) {
    if (n >= cap) {
      break;
    }
    snprintf(out[n].world, sizeof(out[n].world), "%s", json_str(row, "world"));
    snprintf(out[n].name, sizeof(out[n].name), "%s", json_str(row, "name"));
    snprintf(out[n].room, sizeof(out[n].room), "%s", json_str(row, "room"));
    out[n].at_ms = json_ll(row, "at");
    n++;
  }
  cJSON_Delete(result);
  if (out_count != NULL) {
    *out_count = n;
  }
  return 0;
}

int hg_grid_ledger_stats(hg_grid *grid, hg_grid_ledger_kind *out, size_t cap,
                         size_t *out_count, int *out_total) {
  if (out_count != NULL) {
    *out_count = 0;
  }
  if (out_total != NULL) {
    *out_total = 0;
  }
  cJSON *result = NULL;
  if (rpc_call(grid, "ledgerStats", cJSON_CreateArray(), &result) != 0) {
    return -1;
  }
  if (!cJSON_IsArray(result)) {
    cJSON_Delete(result);
    return -1;
  }
  size_t n = 0;
  int total = 0;
  const cJSON *row = NULL;
  cJSON_ArrayForEach(row, result) {
    int count = json_int(row, "count", 0);
    total += count;
    if (n < cap) {
      snprintf(out[n].kind, sizeof(out[n].kind), "%s", json_str(row, "kind"));
      out[n].count = count;
      n++;
    }
  }
  cJSON_Delete(result);
  if (out_count != NULL) {
    *out_count = n;
  }
  if (out_total != NULL) {
    *out_total = total;
  }
  return 0;
}

int hg_grid_prune_ledger(hg_grid *grid, int *removed) {
  cJSON *params = cJSON_CreateArray();
  cJSON *kinds = cJSON_CreateArray();
  cJSON_AddItemToArray(kinds, cJSON_CreateString("ghost"));
  cJSON_AddItemToArray(kinds, cJSON_CreateString("passage"));
  cJSON_AddItemToArray(kinds, cJSON_CreateString("recall"));
  cJSON_AddItemToArray(params, kinds);
  cJSON *result = NULL;
  if (rpc_call(grid, "pruneLedgerKinds", params, &result) != 0) {
    return -1;
  }
  if (removed != NULL) {
    *removed = cJSON_IsObject(result) ? json_int(result, "removed", 0) : 0;
  }
  cJSON_Delete(result);
  return 0;
}
