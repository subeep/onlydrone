#!/usr/bin/env bash
# Targeted DroneID capture: dwell on ONE known DroneID channel for a long time so we
# catch the beacon each time the drone hops to it. Centered exactly on the channel, so
# the decoder needs no frequency offset.
#
# Usage: scripts/capture_droneid.sh <label> [channel_hz] [secs] [gain]
#   e.g. scripts/capture_droneid.sh mini2 2459500000 30 35
#
# Known 2.4 GHz DroneID channels: 2.3995e9 2.4145e9 2.4295e9 2.4445e9 2.4595e9
# Fly the drone the whole time. Receive-only.
set -euo pipefail

LABEL="${1:?drone label}"
CHAN="${2:-2459500000}"   # default 2.4595 GHz (most common)
SECS="${3:-30}"
GAIN="${4:-35}"

BIN="$(dirname "$0")/../build/iq_record"
OUTDIR="$(dirname "$0")/../data/${LABEL}"
mkdir -p "$OUTDIR"
OUT="${OUTDIR}/${LABEL}_droneid_$(printf '%.0f' "$CHAN")_$(date -u +%Y%m%dT%H%M%SZ).iq"

echo ">>> dwell ${LABEL} @ ${CHAN} Hz for ${SECS}s (gain ${GAIN}) -> ${OUT}"
"$BIN" "$OUT" --config "$(dirname "$0")/../config/x310_1gbe_nb.toml" \
  --freq "$CHAN" --gain "$GAIN" --secs "$SECS" --store sc16

echo "decode with:"
echo "  build/droneid_decode '$OUT' --format sc16 --secs ${SECS} --thresh 0.5"
