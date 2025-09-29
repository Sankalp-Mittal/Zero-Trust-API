#!/usr/bin/env bash
set -euo pipefail

# ---- Configurable filenames (edit if your files are named differently) ----
SERVER_SRC="duatallah_pairing_server.cpp"
CLIENT_SRC="duoram_party_client_sync.cpp"
COORD_SRC="coordinator_cli.cpp"

SERVER_BIN="share_server"
CLIENT_BIN="party_client"
COORD_BIN="coordinator_cli"

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++20 -O2 -Wall -Wextra -Wpedantic"
LDFLAGS="-lboost_system -lpthread"

# ---- Checks ----
need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing dependency: $1"
    echo "On Debian/Ubuntu: sudo apt-get install $2"
    exit 1
  fi
}
need tmux tmux
need ${CXX} g++

# If you use Boost from packages:
#   sudo apt-get install libboost-system-dev

echo "Compiling ${SERVER_SRC} -> ${SERVER_BIN}"
${CXX} ${CXXFLAGS} "${SERVER_SRC}" -o "${SERVER_BIN}" ${LDFLAGS}

echo "Compiling ${CLIENT_SRC} -> ${CLIENT_BIN}"
${CXX} ${CXXFLAGS} "${CLIENT_SRC}" -o "${CLIENT_BIN}" ${LDFLAGS}

echo "Compiling ${COORD_SRC} -> ${COORD_BIN}"
${CXX} ${CXXFLAGS} "${COORD_SRC}" -o "${COORD_BIN}" ${LDFLAGS}

echo "Build complete:"
ls -lh "${SERVER_BIN}" "${CLIENT_BIN}" "${COORD_BIN}"
