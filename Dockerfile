# syntax=docker/dockerfile:1

ARG DEBIAN_TAG=trixie-slim

FROM debian:${DEBIAN_TAG} AS build

ARG DUCKDB_VERSION=1.5.5

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        clang \
        g++ \
        ninja-build \
        ca-certificates \
        curl \
        unzip; \
    rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    arch="$(dpkg --print-architecture)"; \
    case "$arch" in \
        amd64) duckdb_arch=amd64 ;; \
        arm64) duckdb_arch=arm64 ;; \
        *) echo "unsupported architecture: $arch" >&2; exit 1 ;; \
    esac; \
    url="https://github.com/duckdb/duckdb/releases/download/v${DUCKDB_VERSION}/libduckdb-linux-${duckdb_arch}.zip"; \
    curl -fsSL "$url" -o /tmp/libduckdb.zip; \
    unzip -d /tmp/libduckdb /tmp/libduckdb.zip; \
    install -m 0644 /tmp/libduckdb/duckdb.h /tmp/libduckdb/duckdb.hpp /usr/local/include/; \
    install -m 0755 /tmp/libduckdb/libduckdb.so /usr/local/lib/; \
    rm -rf /tmp/libduckdb /tmp/libduckdb.zip; \
    ldconfig

WORKDIR /src
COPY . .

RUN set -eux; \
    sed -i 's|^con_prefix = .*|con_prefix = /usr/local|' build.ninja; \
    ninja build/dev/xa6; \
    ldd build/dev/xa6

FROM debian:${DEBIAN_TAG} AS runtime

RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends libstdc++6; \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/lib/libduckdb.so /usr/local/lib/libduckdb.so
COPY --from=build /src/build/dev/xa6 /usr/local/bin/xa6
RUN ldconfig

WORKDIR /work
ENTRYPOINT ["xa6"]
