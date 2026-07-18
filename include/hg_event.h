#ifndef HG_EVENT_H
#define HG_EVENT_H

#include <cjson/cJSON.h>

char *hg_event_line(const char *name, cJSON *payload);

#endif
