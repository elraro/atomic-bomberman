#!/bin/sh
# Fetches SDL (the version the desktop builds use) into android/SDL: its C code
# is built into the app, and its Java sources are the app's activity.
set -e
cd "$(dirname "$0")"
TAG=release-3.2.8
if [ -d SDL/.git ]; then
    echo "android/SDL is already there"
else
    git clone --depth 1 --branch "$TAG" https://github.com/libsdl-org/SDL.git SDL
fi
