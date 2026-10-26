# Third-party code

This project is **GPL-3.0-or-later**. Vendored components:

## turbofec — `third_party/turbofec/`
- Source: https://github.com/ttsou/turbofec
- License: **GPL-3.0** (see `third_party/turbofec/LICENSE`)
- Use: LTE turbo encoder/decoder + rate matching (`src/fec/turbo.cpp` wraps it).
- Note: the turbo decoder core is only defined under `HAVE_SSE3`; our CMake enables SSE3/SSE4.1
  on x86. Without it the decoder silently returns zeros.

## CRC++ (CRCpp) — `third_party/crcpp/CRC.h`
- Source: https://github.com/d-bahr/CRCpp
- License: **BSD-3-Clause** (see `third_party/crcpp/LICENSE`)
- Use: CRC-24-LTEA computation (`src/fec/turbo.cpp`).

## Algorithm reference (not vendored, ported with attribution)
- https://github.com/proto17/dji_droneid — GPL. The DSP/descramble/turbo chain in
  `src/dji/` and `src/fec/` is ported from its `matlab/updated_scripts` and `cpp/*.cc`.
  Credits in that repo to RUB-SysSec / CISPA for the DroneID reverse engineering.

## Real-IQ test fixture — `tests/data/mini2_centered.sc16`
- Source: RUB-SysSec [DroneSecurity](https://github.com/RUB-SysSec/DroneSecurity),
  revision `9ff819843bee48fb140a0704ec78aff757896dea`, `samples/mini2_sm`.
- Upstream license: **AGPL-3.0**, retained in `tests/data/LICENSE.DroneSecurity`.
- Use: centered/resampled excerpt for decoder regression testing; provenance,
  transformation and checksums are in `tests/data/README.md`.
- The upstream packed record layout was also consulted to verify v2 field
  boundaries against the decoded bytes. Motion units still require validation.
