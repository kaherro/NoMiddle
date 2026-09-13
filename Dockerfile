FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        pkg-config \
        ca-certificates \
        libsqlite3-dev \
        libcurl4-openssl-dev \
        libssl-dev \
        libsodium-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

COPY CMakeLists.txt ./
COPY src/ ./src/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
        -DCPR_USE_SYSTEM_CURL=ON \
        -DCPR_BUILD_TESTS=OFF \
    && cmake --build build -j"$(nproc)"

FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libsqlite3-0 \
        libcurl4 \
        libssl3 \
        libsodium23 \
        openssl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=build /workspace/build/server /app/server
COPY --from=build /workspace/build/_deps/cpr-build/cpr/libcpr.so* /app/lib/
COPY web/ /app/web/

ENV LD_LIBRARY_PATH=/app/lib

RUN openssl req -x509 -newkey rsa:2048 -sha256 -nodes \
        -days 365 \
        -keyout /app/key.pem \
        -out /app/cert.pem \
        -subj "/C=US/O=NoMiddle/CN=localhost" \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

EXPOSE 18080

CMD ["./server", "18080", "/data/NoMiddle.db", "cert.pem", "key.pem"]