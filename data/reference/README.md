# Public wideband reference

The local `mini2_sm.cf32` is an unmodified copy of RUB-SysSec DroneSecurity's
[`samples/mini2_sm`](https://github.com/RUB-SysSec/DroneSecurity/blob/9ff819843bee48fb140a0704ec78aff757896dea/samples/mini2_sm)
at revision `9ff819843bee48fb140a0704ec78aff757896dea`. It is interleaved little-endian
float32 I/Q at 50 MSPS. The upstream AGPL-3.0 license is retained at
`tests/data/LICENSE.DroneSecurity`.

The full file is gitignored. To restore it:

```bash
curl -fL https://raw.githubusercontent.com/RUB-SysSec/DroneSecurity/9ff819843bee48fb140a0704ec78aff757896dea/samples/mini2_sm -o data/reference/mini2_sm.cf32
sha256sum data/reference/mini2_sm.cf32
build/droneid_decode data/reference/mini2_sm.cf32 --format cf32 --rate 50000000 --auto-center --secs 0
```

Expected SHA-256: `5a17913cb11cde2919347054700345c9e5595eaff7818980d332b8ac8910bc55`.
Verified result on 2026-10-26: shift **-9,671,021 Hz**, **10 candidates / 10 CRC-valid**,
with LPF on or off. Explicit `--freq-offset -9600000` also decodes 10/10.
These are offline results; no live hardware was used for this validation.

The resampler keeps FIR history and an absolute output time grid across reads,
including fractional ratios such as 50/15.36. Its symmetric window compensates
filter delay, and reported detection indices are mapped back to input samples.
Memory is bounded by the preview, FIR history and decoder chunk, not file length.

Automatic centering uses an averaged 8192-point Hann-windowed spectrum over a
bounded prefix. It selects a non-wrapping 10 MHz band; it is a single-channel
heuristic, not a wideband multi-drone detector. Use an explicit shift in clutter.
The prefix must contain useful signal; fewer than 8192 input samples leave the
shift at zero. Centering is not revisited later in the file.
