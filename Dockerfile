FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake g++ ninja-build libsodium-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B /build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DEDF2027_HEADLESS_SERVER_ONLY=ON \
    && cmake --build /build --target edf2027_server edf2027_master --parallel \
    && cmake --install /build --prefix /install

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 libsodium23 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --user-group --uid 10001 --no-create-home --home-dir /data \
        --shell /usr/sbin/nologin edf \
    && mkdir -p /data/scripts /state \
    && chown -R edf:edf /state

COPY --from=build /install/edf2027_server /usr/local/bin/edf2027_server
COPY --from=build /install/edf2027_master /usr/local/bin/edf2027_master
COPY LICENSE /usr/local/share/licenses/edf2027/LICENSE
COPY LICENSES /usr/local/share/licenses/edf2027

WORKDIR /data
USER 10001:10001
EXPOSE 27717/udp 27718/udp 27719/udp

ENTRYPOINT ["/usr/local/bin/edf2027_server"]
CMD ["--name", "EDF2027 Server", "--data-dir", "/data", "--identity-file", "/state/identity.edfkey"]
