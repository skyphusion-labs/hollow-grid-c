# syntax=docker/dockerfile:1
#
# Ferrite Wastes -- Hollow Grid world server (C port). Multi-stage: build against
# Ubuntu 24.04 (same deps as CI), then ship the binary on a slim runtime image
# with the shared libraries it needs (libwebsockets, cJSON, libcurl, OpenSSL).

# --- build ---
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential pkg-config \
      libcjson-dev libwebsockets-dev libcurl4-openssl-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY include include
COPY src src
COPY tests tests
RUN make -j"$(nproc)"

# --- run ---
FROM ubuntu:24.04 AS run
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      libcjson1 libwebsockets19t64 libcurl4t64 libssl3t64 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/hollow-grid-c /usr/local/bin/hollow-grid-c
VOLUME ["/data"]
EXPOSE 8792
ENTRYPOINT ["/usr/local/bin/hollow-grid-c"]
# Overridable via flags or env (LISTEN_ADDR, DATA_DIR, WORLD_NAME, WORLD_URL,
# GRID_HUB_URL, GRID_HUB_TOKEN). See compose.yaml and .env.example.
CMD ["--addr", "0.0.0.0:8792", "--data", "/data", "--world-name", "Ferrite Wastes"]
