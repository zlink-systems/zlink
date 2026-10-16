ARG UBUNTU_IMAGE=ubuntu:24.04
ARG DOCKER_CLI_IMAGE=docker:28.5.1-cli
FROM ${UBUNTU_IMAGE} AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl zip unzip tar pkg-config \
    autoconf automake libtool python3 ca-certificates linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY vcpkg.json CMakePresets.json ./
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout "$(python3 -c 'import json; print(json.load(open("vcpkg.json"))["builtin-baseline"])')" \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && git clone --depth 1 --branch "framework-cpp/v$(python3 -c 'import json; print(json.load(open("vcpkg.json"))["overrides"][0]["version"])')" \
       https://github.com/zlink-systems/zlink.git /opt/zlink
ENV VCPKG_ROOT=/opt/vcpkg VCPKG_OVERLAY_PORTS=/opt/zlink/vcpkg/ports
RUN /opt/vcpkg/vcpkg install --triplet x64-linux
COPY CMakeLists.txt ./
COPY server server
COPY e2e e2e
RUN cmake --preset default && cmake --build --preset default \
    && mkdir -p /out/bin /out/lib \
    && cp build/bin/my-game-* /out/bin/ \
    && find vcpkg_installed/x64-linux/lib -name '*.so*' -exec cp -P '{}' /out/lib/ ';'

FROM ${UBUNTU_IMAGE} AS node
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libstdc++6 libgcc-s1 libatomic1 libssl3t64 zlib1g \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /out /app
ENV LD_LIBRARY_PATH=/app/lib

FROM ${DOCKER_CLI_IMAGE} AS docker-cli
FROM node AS runner
COPY --from=docker-cli /usr/local/bin/docker /usr/local/bin/docker
WORKDIR /template
COPY vcpkg.json zlink.structure.json docker-compose.yml ./
COPY README.md AGENTS.md CLAUDE.md CMakeLists.txt CMakePresets.json run-e2e.sh run-e2e.ps1 ./
COPY server server
COPY e2e e2e
ENTRYPOINT ["/app/bin/my-game-e2e", "--root", "/template"]
