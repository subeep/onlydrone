#!/usr/bin/env bash
# punjdrone capture helper — record DroneID candidate channels for one band.
# Usage: scripts/capture.sh <24|58> <drone-label> [secs] [gain]
#   e.g. scripts/capture.sh 24 mini2 20 30
#        scripts/capture.sh 58 air3s 20 40
#
# Records a short file per candidate center frequency into data/<label>/.
# These centers are STARTING POINTS — refine them from burst_scan output.
# Fly the drone (motors armed / controller linked) while this runs: DroneID may
# only transmit in flight. Receive-only; nothing is transmitted.
set -euo pipefail

BAND="${1:?band: 24 or 58}"
LABEL="${2:?drone label, e.g. mini2}"
SECS="${3:-20}"
GAIN="${4:-30}"

BIN="$(dirname "$0")/../build/iq_record"
[ -x "$BIN" ] || { echo "build iq_record first (cmake --build build)"; exit 1; }

OUTDIR="$(dirname "$0")/../data/${LABEL}"
mkdir -p "$OUTDIR"

case "$BAND" in
  24) # 2.4 GHz ISM — DroneID hops across the band; sample a few 15.36 MHz slices.
      CENTERS=(2410e6 2425e6 2440e6 2455e6 2470e6) ;;
  58) # 5.8 GHz — wider; sample across the band.
      CENTERS=(5745e6 5775e6 5805e6 5835e6) ;;
  *)  echo "band must be 24 or 58"; exit 1 ;;
esac

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
for C in "${CENTERS[@]}"; do
  OUT="${OUTDIR}/${LABEL}_${BAND}_${C}_${STAMP}.iq"
  echo ">>> ${LABEL} @ ${C} for ${SECS}s (gain ${GAIN}) -> ${OUT}"
  # store sc16 to halve file size; use the repo default profile for device/rate.
  "$BIN" "$OUT" \
    --config "$(dirname "$0")/../config/x310_1gbe_nb.toml" \
    --freq "$C" --gain "$GAIN" --secs "$SECS" --store sc16
done

echo "done. Analyze with: build/burst_scan ${OUTDIR}/<file>.iq"
