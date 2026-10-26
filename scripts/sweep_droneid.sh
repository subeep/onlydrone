#!/usr/bin/env bash
# Sweep ALL known DroneID channels: capture each one centered (so the proven 15.36 MSPS
# decode path is used, no channelizer), then decode each. Catches the beacon regardless of
# which channel the drone hops to.
#
# Usage: scripts/sweep_droneid.sh <label> [band 24|58] [secs_per_channel] [gain]
#   e.g. scripts/sweep_droneid.sh mini2 24 8 35
#
# Fly the drone the WHOLE time (keep it airborne for all channels). Receive-only.
# IMPORTANT: DroneID only broadcasts once the drone has a GPS fix — fly outdoors with a
# clear sky view until the app shows GPS/home-point ready, or no channel will carry it.
set -euo pipefail

LABEL="${1:?drone label}"
BAND="${2:-24}"
SECS="${3:-8}"
GAIN="${4:-35}"

HERE="$(dirname "$0")"
REC="$HERE/../build/iq_record"
DEC="$HERE/../build/droneid_decode"
CFG="$HERE/../config/x310_1gbe_nb.toml"
OUTDIR="$HERE/../data/${LABEL}/sweep_$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$OUTDIR"

case "$BAND" in
  24) CHANS=(2399500000 2414500000 2429500000 2444500000 2459500000) ;;
  58) CHANS=(5756500000 5776500000 5796500000) ;;
  *)  echo "band must be 24 or 58"; exit 1 ;;
esac

echo "=== capturing ${#CHANS[@]} channels x ${SECS}s (fly the whole time) ==="
FILES=()
for C in "${CHANS[@]}"; do
  F="${OUTDIR}/${LABEL}_$(printf '%.0f' "$C").iq"
  echo ">>> capture @ ${C} Hz -> ${F}"
  "$REC" "$F" --config "$CFG" --freq "$C" --gain "$GAIN" --secs "$SECS" --store sc16 \
    2>&1 | grep -Ei "done|overflow" || true
  FILES+=("$F")
done

echo; echo "=== decoding each channel ==="
HITS=0
for F in "${FILES[@]}"; do
  echo "--- $(basename "$F") ---"
  OUT="$("$DEC" "$F" --format sc16 --secs "$SECS" --thresh 0.5 2>/dev/null)"
  echo "$OUT" | grep -E "serial|drone |pilot |== " || true
  echo "$OUT" | grep -q "CRC-valid ==" && echo "$OUT" | grep -q "[1-9] CRC-valid" && HITS=$((HITS+1)) || true
done
echo; echo "=== sweep done: ${HITS} channel(s) with a CRC-valid frame ==="
echo "raw captures kept in ${OUTDIR}"
