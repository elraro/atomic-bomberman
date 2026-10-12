# The master server of online play and its key tool (abconfig), in one small image.
#   docker build -t atomic-bomberman-master master/
# How it is run with the relay and the database: ../deploy/README.md.
FROM golang:1.25-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/ ./cmd/...

FROM alpine:3.22
LABEL org.opencontainers.image.source="https://github.com/elraro/atomic-bomberman" \
      org.opencontainers.image.licenses="GPL-3.0-only"
COPY --from=build /out/master /out/abconfig /usr/local/bin/
USER 65534:65534
EXPOSE 8443
ENTRYPOINT ["master"]
