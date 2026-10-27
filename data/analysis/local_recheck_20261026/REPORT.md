# Local capture recheck — 26 October 2026

Checked all **21 local `.iq` recordings** in `data/` (11.37 GB, approximately
185 seconds of recorded IQ). The newest local recording found was
`mini2/sweep_20261026T092618Z/mini2_2459500000.iq`, modified at **14:57:16 IST**.
The downloaded public reference was tested separately as a positive control.

## Results

| Capture group | Files | Recorded seconds | Full-file decode passes | Candidate hits across passes | CRC-valid |
|---|---:|---:|---:|---:|---:|
| Mini 2 | 11 | 110.0032 | 26 | 0 | 0 |
| Air 3S | 9 | 72 | 23 | 183 | 0 |
| No-drone baseline | 1 | 3.0037 | 2 | 0 | 0 |
| Public Mini 2 reference control | 1 | 0.01455 | 1 | 10 | 10 |

Candidate hits are summed across different processing passes and are **not unique
packets or drones**. None of the local candidate payloads passed the legacy
DroneID decoder's CRC-24 check. The Mini 2 result includes the full 30-second
dwell and all five most recent channel-centered sweep files.

## Method and integrity

- Every local file was decoded in full (`--secs 0 --thresh 0.3`) at its recorded
  center and with automatic centering: 42 full-file passes.
- Nine additional full-file passes used known-channel frequency shifts: -4.5 MHz
  for the 2410/2425/2440/2455 MHz slices, and -1.5 MHz for the 5775 MHz Air 3S slice.
- Each file also received a diagnostic frequency scan over its first 1,048,576
  samples (about 68.27 ms at 15.36 MSPS), from -2.5 to +2.5 MHz in 0.5 MHz steps.
  These scans are prefix diagnostics, not exhaustive frequency/time searches.
- All 72 local commands succeeded. Files remained unchanged during the check;
  all lengths contained complete samples. All sidecars reported sc16, 15.36 MSPS
  and zero capture overflows. These are recorded metadata, not a new radio test.
- The public 50-MSPS reference, decoded with `--auto-center --secs 0`, passed
  **10/10** as a positive control during this run.

Commands, file settings, selected shifts, individual counts and log paths are in
[results.json](results.json). Complete stdout/stderr is under [logs](logs/).
The control output is in [reference_control.log](reference_control.log).

Reproduce from the project root:

```bash
python3 scripts/check_captures.py --output data/analysis/local_recheck_20261026
```

## Air 3S candidates require a different interpretation

The strongest activity was in the 2425 MHz and 5745 MHz Air 3S recordings.
The 2425 MHz automatic-centering pass produced 72 candidates; the 5745 MHz
recorded-center pass produced 70. Some candidate pairs are separated by 1096
samples, one normal OFDM symbol interval at this rate. This structure alone does
not establish that they are legacy DroneID frames.

Two examples were checked against **both** expected ZC symbols, searching timing
within the extracted window and integer-carrier CFO over approximately ±7.5 MHz:

| Example | Input window index | Initial shift | Root-600 score at best pair | Root-147 score at best pair | Product |
|---|---:|---:|---:|---:|---:|
| Included reference fixture | 2384 | 0 Hz | 0.9836 | 0.9843 | 0.968153 |
| Air 3S 2425 MHz | 1028217 | +2,681,250 Hz | 0.5286 | 0.0091 | 0.004814 |
| Air 3S 5745 MHz | 6739585 | 0 Hz | 0.6740 | 0.0065 | 0.004378 |

These two examples match the first sequence but poorly match the required second
sequence at the expected separation. They are **unconfirmed legacy-format
candidates**, not decoded identities or locations. This does not classify every
Air 3S hit or rule out a different O4 waveform.

## Limits and next useful capture

- Mini 2 prefix-scan maxima ranged from 0.0232 to 0.1036. No full-file pass found
  a candidate at threshold 0.3. This is a decoder result, not proof that the drone
  emitted no signal.
- Several early slices do not fully contain a nominal 10 MHz DroneID channel.
  For example, a +4.5 MHz offset in a 15.36-MSPS recording extends beyond the
  +7.68 MHz capture edge; postprocessing cannot restore those missing frequencies.
  The five recent channel-centered Mini 2 files do provide full nominal bandwidth.
- Automatic centering selects one strong band from a short prefix; it cannot
  follow hopping signals or distinguish interference. Legacy decoding does not
  establish support for Air 3S/O4.
- The local Mini 2 files here cover 2.4 GHz only. A future centered 5.8 GHz Mini 2
  capture would add coverage not present in this dataset. The present run was
  entirely offline and neither changed radio settings nor transmitted anything.
