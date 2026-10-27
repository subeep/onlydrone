# DJI DroneID IQ test samples

Real-world IQ captures to test the decoder against, one folder per drone model.

**Reality check:** public *DroneID-frame* IQ recordings are scarce. The frames contain the
drone's and pilot's GPS, so almost nobody publishes raw captures. After searching
(RUB-SysSec, proto17, anarkiwi, antsdr, Kismet, Zenodo/CardRF/DroneRF, OcuSync research repos),
the only publicly **downloadable, decodable DroneID IQ** are the two RUB-SysSec samples below.
"All DJI drones" is therefore not obtainable publicly — the rest can only be tested with your own
captures (outdoors, GPS-locked). The entries marked "no public IQ" are listed so the coverage gap
is explicit.

Everything here is receive/offline test data. No code was changed to produce this folder.

## Available captures (downloaded)

| Folder | Drone | DroneID gen | Rate / format | Our decode result |
|---|---|---|---|---|
| `mini2/mini2_sm.cf32` | **DJI Mini 2** | OcuSync 2.0 | 50 MSPS, cf32 | **10/10 bursts CRC-valid** — serial `SysSecWasHere`, pilot GPS, timestamp ✓ |
| `mavic_air_2/mavic_air_2.cf32` | **DJI Mavic Air 2** | OcuSync 2.0 | 50 MSPS, cf32 | **1/1 burst CRC-valid** after carrier-offset search and filter-delay fixes; version 2 payload parsed |

Both are from RUB-SysSec [DroneSecurity](https://github.com/RUB-SysSec/DroneSecurity) `samples/`,
revision `9ff819843bee48fb140a0704ec78aff757896dea`. Upstream license **AGPL-3.0**
(`LICENSE.DroneSecurity`). These are concatenated candidate bursts, not continuous recordings.

SHA-256:
- `mini2_sm.cf32`     — `5a17913cb11cde2919347054700345c9e5595eaff7818980d332b8ac8910bc55`
- `mavic_air_2.cf32`  — `623fa40f190d3fd859ec2fd62b379ad9a01a0485db3179ae29ac2e3c7a070450`

Decode (whole file, auto-center the ~10 MHz band, resample 50→15.36 MSPS internally):
```bash
build/droneid_decode data/iq_samples/mini2/mini2_sm.cf32        --format cf32 --rate 50000000 --auto-center --secs 0
build/droneid_decode data/iq_samples/mavic_air_2/mavic_air_2.cf32 --format cf32 --rate 50000000 --auto-center --secs 0
```

### Mavic Air 2 failure diagnosis (2026-10-26)

Automatic band centering applies +11.984253 MHz, leaving about -485.94 kHz
residual offset. The old joint-ZC alignment searched only ±16 carriers (±240 kHz)
around the CP estimate, excluding the correct solution. The default low-pass
filter also added an uncompensated 25-sample delay, pushing the correct frame
start beyond the alignment window. Expanding the search to ±64 carriers
(±960 kHz) and compensating that delay recovers the CRC-valid payload with
filtering either enabled or disabled, using the existing version-2 parser.
The earlier attribution to legacy/frame-version
handling was incorrect for this sample. An explicit +12.470193 MHz input shift
also decoded it with the old search limit.

## DJI models and DroneID coverage (reference list)

Decodable family (physical-layer Gold-code scrambling, no AES):

| Drone | DroneID gen | Public IQ? | Notes |
|---|---|---|---|
| Mini 2 | OcuSync 2.0 | **yes (here)** | fully decoded |
| Mavic Air 2 | OcuSync 2.0 | **yes (here)** | decoded; residual-frequency search limit fixed |
| Mavic 2 (Pro/Zoom) | OcuSync 2.0 | no | same family, expected decodable |
| Air 2S | OcuSync 3.0 | no | expected decodable |
| Mini 3 / Mini 3 Pro | OcuSync 3 / O3 | no | expected decodable |
| Mavic 3 / Classic / Pro / Enterprise | O3+ | no | expected decodable |
| Mini SE / Mavic Air / Spark / Phantom 4 | Wi-Fi DroneID / OcuSync 1 | no | Wi-Fi-beacon variant or least-documented O1 |

Detect-only family (O4, AES-encrypted payload — will NOT decode by design):

| Drone | Gen | Public IQ? | Notes |
|---|---|---|---|
| Air 3 / Air 3S | O4 | no (you have local Air 3S captures) | detect + direction only |
| Mini 4 Pro | O4 | no | detect only |
| Avata 2 | O4 | no | detect only |

## Getting your own (for the "no public IQ" rows)

Capture outdoors with the drone **GPS-locked**, then:
```bash
scripts/sweep_droneid.sh <label> 24 8 35     # all five 2.4 GHz channels
build/droneid_decode data/<label>/sweep_*/<file>.iq --format sc16 --secs 8 --thresh 0.5
```
Drop the resulting `.iq` into a new `data/iq_samples/<model>/` folder and add a row above.
