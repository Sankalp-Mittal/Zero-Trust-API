#!/usr/bin/env bash
set -euo pipefail

SESSION="duoram"
# Default ports/hosts (change if ports are in use)
SERVER_ADDR="0.0.0.0:9300"   # share server
A_LISTEN="0.0.0.0:9700"      # Party A user request port
A_PEER_LISTEN="9701"         # Party A peer residual inbound
B_LISTEN="0.0.0.0:9800"      # Party B user request port
B_PEER_LISTEN="9801"         # Party B peer residual inbound

A_PEER_TARGET="127.0.0.1:9801"  # A will send residuals to B’s peer-listen
B_PEER_TARGET="127.0.0.1:9701"  # B will send residuals to A’s peer-listen

ROWS="${ROWS:-1024}"            # DUORAM rows (can override: ROWS=2048 ./run_tmux.sh)

SERVER_BIN="./share_server"
CLIENT_BIN="./party_client"
COORD_BIN="./coordinator_cli"

# Ensure tmux exists
if ! command -v tmux >/dev/null 2>&1; then
  echo "tmux not found. Install it (e.g., sudo apt-get install tmux) and re-run."
  exit 1
fi

# Kill any old session with the same name
tmux has-session -t "${SESSION}" 2>/dev/null && tmux kill-session -t "${SESSION}"

# Create a new session, window 0, pane 0: Share Server
tmux new-session -d -s "${SESSION}" -n "server"
tmux send-keys -t "${SESSION}":0.0 "echo 'Starting Share Server on ${SERVER_ADDR}'" C-m
tmux send-keys -t "${SESSION}":0.0 "${SERVER_BIN} --listen ${SERVER_ADDR}" C-m

# Split window 0 vertically for Party A
tmux split-window -v -t "${SESSION}":0
tmux send-keys -t "${SESSION}":0.1 "echo 'Starting Party A...'" C-m
tmux send-keys -t "${SESSION}":0.1 \
  "${CLIENT_BIN} --role A --rows ${ROWS} \
    --listen ${A_LISTEN} \
    --peer-listen ${A_PEER_LISTEN} \
    --peer ${A_PEER_TARGET} \
    --share ${SERVER_ADDR}" C-m

# Split window 0 again (horizontal split on bottom pane) for Party B
tmux split-window -h -t "${SESSION}":0.1
tmux send-keys -t "${SESSION}":0.2 "echo 'Starting Party B...'" C-m
tmux send-keys -t "${SESSION}":0.2 \
  "${CLIENT_BIN} --role B --rows ${ROWS} \
    --listen ${B_LISTEN} \
    --peer-listen ${B_PEER_LISTEN} \
    --peer ${B_PEER_TARGET} \
    --share ${SERVER_ADDR}" C-m

# Optional: create a second tmux window for the Coordinator CLI
tmux new-window -t "${SESSION}" -n "coord"
tmux send-keys -t "${SESSION}":1.0 "echo 'Coordinator window. Examples:'" C-m
tmux send-keys -t "${SESSION}":1.0 \
  "echo './coordinator_cli --op write --dim ${ROWS} --idx 7 --val 12345 --c0 ${A_LISTEN} --c1 ${B_LISTEN}'" C-m
tmux send-keys -t "${SESSION}":1.0 \
  "echo './coordinator_cli --op read  --dim ${ROWS} --idx 7 --c0 ${A_LISTEN} --c1 ${B_LISTEN}'" C-m

# Attach to the tmux session
tmux select-window -t "${SESSION}":0
tmux attach-session -t "${SESSION}"

# ---------
# tmux quick cheatsheet (also echoed below):
# - Switch panes:  Ctrl-b  +  Arrow keys
# - Switch windows: Ctrl-b  +  n (next) / p (prev) / 0..9 (by number)
# - Detach: Ctrl-b  +  d
# - Kill session later: tmux kill-session -t duoram
