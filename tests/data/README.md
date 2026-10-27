# Mini 2 decoder regression fixture

`mini2_centered.sc16` contains the first 16,000 complex samples of the previously
prepared `mini2_ref2.iq` capture: interleaved little-endian signed int16 I/Q,
15.36 MSPS, no sidecar required. It contains one complete reference burst.

Source: RUB-SysSec [DroneSecurity, samples/mini2_sm](https://github.com/RUB-SysSec/DroneSecurity/blob/9ff819843bee48fb140a0704ec78aff757896dea/samples/mini2_sm),
revision `9ff819843bee48fb140a0704ec78aff757896dea`. The upstream repository carries
AGPL-3.0; its license is retained in `LICENSE.DroneSecurity` for this fixture.

The original capture is cf32 at 50 MSPS. The saved preprocessing finds the maximum
power 10 MHz window in its whole-file FFT, shifts that window's center to DC,
retains the bins for 15.36 MSPS, inverse-transforms and divides by the input sample
count, then clips each component to [-1, 1] and quantizes with a scale of 20,000.
This prepared asset remains the baseline for exact payload comparisons.

`mini2_wideband.cf32` is an unmodified excerpt of the first 52,084 complex samples
of the original `mini2_sm`, at 50 MSPS (416,672 bytes). It tests the integrated
centering and streaming resampler against the same exact decoded payload, using
both automatic centering and an explicit -9.6 MHz shift. The same upstream
AGPL-3.0 license applies; this excerpt has no sidecar, so pass `--rate 50000000`.

SHA-256:

- Original `mini2_sm`: `5a17913cb11cde2919347054700345c9e5595eaff7818980d332b8ac8910bc55`
- Prepared `mini2_ref2.iq`: `2f49073e27931350f2f5f453d8fdb8b9edc301d5aa1d1b8b4a397581504afdcd`
- Included fixture: `71af4bf3e732f9f228994657867bccd5d98dad2a897bfe2eab4f8b266cfa3c23`
- Wideband excerpt: `4690634dd93c709c16e71d9a8ccb40662dcdfea78878b55ce23610ae811a9bd6`

Expected: one CRC-24-valid 176-byte payload, version 2, sequence `0x0312`, serial
`SysSecWasHere ` (trailing space), GPS timestamp `1648558221652`, no drone/home
position and phone coordinates approximately 51.447176, 7.266528 degrees.
The residual CFO is approximately -57.47 kHz. The test compares the entire payload,
so a matching CRC alone cannot conceal a changed decode.

The CP estimate aliases this CFO to +2.53 kHz. Root 600 alone can correlate strongly
at the wrong frequency and timing. Both roots must agree at the prescribed symbol
separation to resolve the ambiguity. Synthetic tests also cover positive CFO,
±485.94 kHz residual offsets observed in the Mavic Air 2 investigation, and both
LPF settings. The LPF delay must be compensated to preserve alignment search room.

Run with `ctest --test-dir build -R test_dji --output-on-failure` or inspect using
`build/droneid_decode tests/data/mini2_centered.sc16 --format sc16 --rate 15360000`.
