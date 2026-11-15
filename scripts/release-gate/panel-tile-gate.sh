#!/bin/bash
# Panel overview tile gate, one invocation per catalogue mode (a run that is killed loses one mode, not all five).
#
#   panel-tile-gate.sh <label> <panel-port> <host-port> <ui-tree> <out-dir>
#
# Needs, in the environment:
#   GATE_DIR   a checkout of the tile checker (it provides checks/tile-fit.mjs); the checker walks the overview's tiles at
#              360-1920 px, light and dark, idle and streaming, the smart plan and the manual s/m/l/xl sizes, and reports
#              text that is cut, stacked one character per line, overlapping, covered or outside its plate
# and, already running:
#   the Panel dev server on <panel-port> over the Panel tree under test and the foundation-ui tree <ui-tree>,
#   a host stand-in on <host-port> that answers with copies of a real host's data (apps, clients, performance, network),
#   and a Tauri bridge stand-in that answers every command the Panel calls at start (a command left unanswered keeps the
#   Panel on "connecting to the host" and the overview never mounts; an exit on the .m-dash__cell timeout means nothing was tested).
#
# Result per mode: <out-dir>/tile-fit-<mode>.json (findings) and .log; <out-dir>/batch.log has the exit codes
# (0 clean, 1 findings; anything else is retried once).
set -u
LABEL=$1; PP=$2; HP=$3; UI=$4; O=$5
: "${GATE_DIR:?set GATE_DIR to the tile checker checkout}"
mkdir -p "$O"
cd "$GATE_DIR" || exit 2
for MODE in smart manual-s manual-m manual-l manual-xl; do
  for TRY in 1 2; do
    node checks/tile-fit.mjs --panel "http://127.0.0.1:$PP" --host-port "$HP" --ui "$UI" --modes "$MODE" --out "$O/tile-fit-$MODE.json" > "$O/tile-fit-$MODE.log" 2>&1
    rc=$?
    echo "$LABEL $MODE try $TRY rc=$rc $(date +%H:%M:%S)" >> "$O/batch.log"
    [ $rc -le 1 ] && break
  done
done
echo done >> "$O/batch.log"
