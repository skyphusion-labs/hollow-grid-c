#include "hg_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *hg_event_line(const char *name, cJSON *payload) {
  if (name == NULL || payload == NULL) {
    cJSON_Delete(payload);
    return NULL;
  }
  char *json = cJSON_PrintUnformatted(payload);
  cJSON_Delete(payload);
  if (json == NULL) {
    return NULL;
  }
  size_t length = strlen(name) + strlen(json) + 12;
  char *line = malloc(length);
  if (line != NULL) {
    snprintf(line, length, "@event %s %s\r\n", name, json);
  }
  cJSON_free(json);
  return line;
}
