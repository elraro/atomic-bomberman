# A dedicated game server (atomic_server), with atomic_verify (checks match records) and
# net_bot (a test client) beside it. No window, no SDL, no original game files.
# Built from the repository's root:
#   docker build -f deploy/gameserver/Dockerfile -t atomic-bomberman-server .
FROM ubuntu:24.04 AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake ninja-build git ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src src
COPY tools tools
COPY tests tests
# Where the signed configuration of online play is and the key that signs it, built in
# (docs/operations/online.md, section 4). Empty: given when the server is started.
ARG AB_ONLINE_CONFIG_URLS=""
ARG AB_ONLINE_CONFIG_KEY=""
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        "-DAB_ONLINE_CONFIG_URLS=${AB_ONLINE_CONFIG_URLS}" "-DAB_ONLINE_CONFIG_KEY=${AB_ONLINE_CONFIG_KEY}" \
    && cmake --build /build --target atomic_server atomic_verify net_bot

FROM ubuntu:24.04
COPY --from=build /build/atomic_server /build/atomic_verify /build/net_bot /usr/local/bin/
# The server's keys, its match records and its list of bans are kept here
# (in /data/atomic-bomberman-modern/atomic/).
ENV XDG_DATA_HOME=/data
VOLUME /data
USER 65534:65534
EXPOSE 27410/udp 27410/tcp
ENTRYPOINT ["atomic_server"]
