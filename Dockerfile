FROM debian:buster-slim AS builder
LABEL maintainer="zlink Project <ulalax@kairoscode.dev>"
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq \
    && apt-get install -qq --yes --no-install-recommends \
        build-essential \
        cmake \
        git \
        libssl-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/zlink
COPY . .
RUN cmake -S . -B core/build/local -DWITH_TLS=ON -DBUILD_TESTS=ON \
    && cmake --build core/build/local \
    && ctest --test-dir core/build/local --output-on-failure \
    && cmake --install core/build/local --prefix /usr/local

FROM debian:buster-slim
LABEL maintainer="zlink Project <ulalax@kairoscode.dev>"
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq \
    && apt-get install -qq --yes --no-install-recommends \
    && rm -rf /var/lib/apt/lists/*
COPY --from=builder /usr/local /usr/local
RUN ldconfig && ldconfig -p | grep libzlink
