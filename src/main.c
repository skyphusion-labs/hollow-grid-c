#include <stdio.h>
#include <string.h>

static void usage(const char *argv0) {
  fprintf(stderr,
          "hollow-grid-c -- Hollow Grid world server (C port)\n"
          "\n"
          "Usage: %s [--help]\n"
          "\n"
          "Scaffold only. Phase 0 will add:\n"
          "  --addr HOST:PORT     listen for /ws (default 0.0.0.0:8792)\n"
          "  --world-name NAME    federation display name\n"
          "  --data DIR           SQLite / content path\n"
          "  GRID_HUB_URL         optional federation hub\n"
          "\n"
          "Contract: the-hollow-grid/docs/protocol.md\n"
          "Done bar: upstream smoke.mjs\n"
          "World: Ferrite Wastes (docs/WORLD.md)\n",
          argv0);
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      usage(argv[0]);
      return 0;
    }
  }

  if (argc > 1) {
    fprintf(stderr, "unknown argument: %s (try --help)\n", argv[1]);
    return 2;
  }

  fprintf(stdout,
          "hollow-grid-c scaffold OK. WebSocket world server not implemented "
          "yet.\nSee docs/PLAN.md.\n");
  return 0;
}
