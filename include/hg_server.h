#ifndef HG_SERVER_H
#define HG_SERVER_H

typedef struct {
  const char *host;
  int port;
  const char *world_name;
  const char *world_url;
  const char *data_dir;
  const char *admins;
  const char *grid_hub_url;
  const char *grid_hub_token;
} hg_server_config;

int hg_server_run(const hg_server_config *config);
void hg_server_stop(void);

#endif
