ARG UBUNTU_IMAGE=ubuntu:24.04
ARG DOCKER_CLI_IMAGE=docker:28.5.1-cli
FROM ${UBUNTU_IMAGE} AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY bootstrap.cmake ./
RUN cmake -P bootstrap.cmake
COPY CMakeLists.txt CMakePresets.json ./
COPY server server
COPY e2e e2e
RUN cmake --preset default && cmake --build --preset default \
    && ctest --test-dir build --output-on-failure \
    && mkdir -p /out/bin /out/lib \
    && cp build/bin/my-game-* /out/bin/ \
    && cp -P .zlink/install/lib/*.so* /out/lib/

FROM ${UBUNTU_IMAGE} AS node
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libstdc++6 libgcc-s1 libssl3t64 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /out /app
ENV LD_LIBRARY_PATH=/app/lib

FROM ${DOCKER_CLI_IMAGE} AS docker-cli
FROM node AS runner
COPY --from=docker-cli /usr/local/bin/docker /usr/local/bin/docker
WORKDIR /template
COPY bootstrap.cmake zlink.structure.json docker-compose.yml ./
COPY README.md AGENTS.md CLAUDE.md CMakeLists.txt CMakePresets.json run-e2e.sh run-e2e.ps1 ./
COPY server server
COPY e2e e2e
ENTRYPOINT ["/app/bin/my-game-e2e", "--root", "/template"]
