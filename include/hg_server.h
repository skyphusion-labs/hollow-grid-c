#ifndef HG_SERVER_H
#define HG_SERVER_H

typedef struct {
  const char *host;
  int port;
  const char *world_name;
  const char *data_dir;
} hg_server_config;

int hg_server_run(const hg_server_config *config);
void hg_server_stop(void);

#endif
