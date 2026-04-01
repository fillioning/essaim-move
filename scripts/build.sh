#!/bin/bash
set -e
MODULE_ID="essaim"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
WIN_ROOT="$(cd "$ROOT" && pwd -W 2>/dev/null || pwd)"

docker build -t schwung-builder "$SCRIPT_DIR"
mkdir -p "$ROOT/dist/$MODULE_ID"

# Create container with build command (WORKDIR is /build)
CONTAINER_ID=$(MSYS_NO_PATHCONV=1 docker create schwung-builder \
  bash -c "mkdir -p /build/dist/$MODULE_ID && \
  dos2unix /build/src/dsp/${MODULE_ID}.c 2>/dev/null; \
  aarch64-linux-gnu-gcc \
    -O2 -shared -fPIC -ffast-math -Wall \
    -o /build/dist/${MODULE_ID}/dsp.so \
    /build/src/dsp/${MODULE_ID}.c \
    -lm && \
  cp /build/src/module.json /build/dist/${MODULE_ID}/ && \
  cd /build/dist && tar -czf ${MODULE_ID}-module.tar.gz ${MODULE_ID}/")

# Copy source into container's /build/src (WORKDIR already exists)
docker cp "$WIN_ROOT/src" "$CONTAINER_ID:/build/src"
docker start -a "$CONTAINER_ID"

# Extract artifacts
docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID/dsp.so" "$WIN_ROOT/dist/$MODULE_ID/"
cp "$ROOT/src/module.json" "$ROOT/dist/$MODULE_ID/"
docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID-module.tar.gz" "$WIN_ROOT/dist/"
docker rm "$CONTAINER_ID" > /dev/null

echo "Built: dist/$MODULE_ID-module.tar.gz"
