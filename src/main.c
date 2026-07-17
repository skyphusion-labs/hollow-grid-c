#include "hg_server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
  fprintf(stdout,
          "hollow-grid-c -- Ferrite Wastes world server\n"
          "\n"
          "Usage: %s [options]\n"
          "\n"
          "  --addr HOST:PORT     listen address (default 0.0.0.0:8792)\n"
          "  --world-name NAME    federation display name (default Ferrite Wastes)\n"
          "  --data DIR           character data directory (default data)\n"
          "  --help, -h           show this help\n"
          "\n"
          "Environment: LISTEN_ADDR, WORLD_NAME, DATA_DIR\n"
          "Contract: the-hollow-grid/docs/protocol.md\n",
          argv0);
}

static void stop_server(int signal_number) {
  (void)signal_number;
  hg_server_stop();
}

static const char *env_or(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static int parse_address(const char *address, char *host, size_t host_size,
                         int *port) {
  const char *colon = strrchr(address, ':');
  if (colon == NULL || colon[1] == '\0') {
    return -1;
  }
  char *end = NULL;
  long parsed = strtol(colon + 1, &end, 10);
  if (*end != '\0' || parsed < 1 || parsed > 65535) {
    return -1;
  }
  size_t host_length = (size_t)(colon - address);
  if (host_length == 0) {
    snprintf(host, host_size, "0.0.0.0");
  } else {
    if (host_length >= host_size) {
      return -1;
    }
    memcpy(host, address, host_length);
    host[host_length] = '\0';
  }
  *port = (int)parsed;
  return 0;
}

int main(int argc, char **argv) {
  const char *address = env_or("LISTEN_ADDR", "0.0.0.0:8792");
  const char *world_name = env_or("WORLD_NAME", "Ferrite Wastes");
  const char *data_dir = env_or("DATA_DIR", "data");

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(argv[0]);
      return 0;
    }
    if (i + 1 >= argc) {
      fprintf(stderr, "missing value for %s\n", argv[i]);
      return 2;
    }
    if (strcmp(argv[i], "--addr") == 0) {
      address = argv[++i];
    } else if (strcmp(argv[i], "--world-name") == 0) {
      world_name = argv[++i];
    } else if (strcmp(argv[i], "--data") == 0) {
      data_dir = argv[++i];
    } else {
      fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
      return 2;
    }
  }

  char host[256];
  int port = 0;
  if (parse_address(address, host, sizeof(host), &port) != 0) {
    fprintf(stderr, "invalid listen address: %s\n", address);
    return 2;
  }

  signal(SIGINT, stop_server);
  signal(SIGTERM, stop_server);

  hg_server_config config = {
      .host = host,
      .port = port,
      .world_name = world_name,
      .data_dir = data_dir,
  };
  return hg_server_run(&config);
}
