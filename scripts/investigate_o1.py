#!/usr/bin/env python3
"""Read-only, bounded spectral diagnostics and candidate decoding for the local O1 capture."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy import fft, ndimage

from validate_corpus import packet_record


def regions(samples, rate, offset):
    transform = 1024
    frames = samples[:len(samples) // transform * transform].reshape(-1, transform)
    spectrum = fft.fftshift(fft.fft(frames * np.hanning(transform).astype(np.float32), axis=1), axes=1)
    power = np.abs(spectrum)**2
    smooth = ndimage.uniform_filter(power, size=(3, 3))
    floor = np.percentile(smooth, 25, axis=0)
    floor = np.minimum(floor, np.median(floor) * 2)
    snr = 10 * np.log10((smooth + 1e-20) / (floor[None, :] + 1e-20))
    mask = ndimage.binary_opening(snr > 18, structure=np.ones((1, 3)))
    mask = ndimage.binary_closing(mask, structure=np.ones((1, 5)))
    labels, _ = ndimage.label(mask)
    detected = []
    for bounds in ndimage.find_objects(labels):
        if bounds is None:
            continue
        times, frequencies = bounds
        duration = (times.stop - times.start) * transform / rate
        bandwidth = (frequencies.stop - frequencies.start) * rate / transform
        if duration < 0.00015 or duration > 0.012 or bandwidth < 1e6:
            continue
        detected.append(dict(start_seconds=offset + times.start * transform / rate,
                             duration_seconds=duration, bandwidth_hz=bandwidth,
                             center_offset_hz=((frequencies.start + frequencies.stop) / 2 - transform / 2) * rate / transform,
                             edge=times.start == 0 or times.stop == len(frames)))
    overview = power[:len(power) // 16 * 16].reshape(-1, 16, transform).mean(axis=1)
    return detected, overview


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, default=Path("/media/sudeep/Storage/datasets/Tampere_DroneRF/DJI_mavic_pro_2G.bin"))
    parser.add_argument("--output", type=Path, default=Path("data/analysis/o1_followup_20261027"))
    parser.add_argument("--rate", type=float, default=120e6)
    parser.add_argument("--decode", action="store_true")
    args = parser.parse_args()
    if not args.input.is_file() or not 10e6 <= args.rate <= 250e6:
        parser.error("existing input and rate in 10–250 MSPS required")
    if args.input.stat().st_size % 4 or args.input.stat().st_size < 32768 * 4:
        parser.error("input must contain at least 32768 complete interleaved int16 IQ samples")
    args.output.mkdir(parents=True, exist_ok=True)
    raw = np.memmap(args.input, dtype="<i2", mode="r").reshape(-1, 2)
    count = min(len(raw), int(2 * args.rate))
    block = int(0.1 * args.rate)
    detected, overview = [], []
    digest = hashlib.sha256()
    for start in range(0, count, block):
        values = raw[start:min(count, start + block)]
        digest.update(values.tobytes())
        samples = (values[:, 0].astype(np.float32) + 1j * values[:, 1].astype(np.float32)) / 32768
        found, display = regions(samples, args.rate, start / args.rate)
        detected.extend(found)
        overview.append(display)
    power = np.concatenate(overview)
    decibels = 10 * np.log10(power + 1e-20)
    figure, axis = plt.subplots(figsize=(15, 6))
    displayed = axis.imshow(decibels.T, origin="lower", aspect="auto", extent=(0, count/args.rate, -args.rate/2e6, args.rate/2e6),
                            vmin=float(np.percentile(decibels, 20)), vmax=float(np.percentile(decibels, 99.7)), cmap="magma")
    axis.set(xlabel="Time (s)", ylabel="Offset from unknown capture center (MHz)", title="Tampere Mavic Pro: observed RF activity, not DroneID labels")
    figure.colorbar(displayed, ax=axis, label="Relative spectral power (dB)")
    figure.tight_layout()
    figure.savefig(args.output / "spectrum.png", dpi=120)
    plt.close(figure)
    candidates = [row for row in detected if not row["edge"] and 4e6 <= row["bandwidth_hz"] <= 13e6
                  and 0.0003 <= row["duration_seconds"] <= 0.0015]
    summary = dict(input=str(args.input), rate=args.rate, center=None, tested_samples=count,
                   tested_seconds=count/args.rate, raw_prefix_sha256=digest.hexdigest(), regions=detected,
                   legacy_shape_candidates=candidates,
                   limitation="Energy-connected regions are search hints, not protocol detection; overlapped/weak bursts can be missed.")
    (args.output / "spectral_regions.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{len(detected)} RF regions, {len(candidates)} legacy-shape search candidates", flush=True)
    if not args.decode:
        return
    repo = Path(__file__).resolve().parents[1]
    results = []
    packets = {}
    retained_clip = None
    with tempfile.TemporaryDirectory(prefix="o1-search-") as temporary:
        for index, candidate in enumerate(candidates[:100]):
            begin = max(0, int((candidate["start_seconds"] - 0.0004) * args.rate))
            end = min(count, int((candidate["start_seconds"] + candidate["duration_seconds"] + 0.0004) * args.rate))
            values = raw[begin:end]
            samples = (values[:, 0].astype(np.float32) + 1j * values[:, 1].astype(np.float32)) / 32768
            path = Path(temporary) / "candidate.cf32"
            samples.astype("<c8").tofile(path)
            for delta in [-1e6, -0.5e6, 0, 0.5e6, 1e6]:
                shift = -candidate["center_offset_hz"] + delta
                command = [str(repo / "build/droneid_decode"), str(path), "--format", "cf32", "--rate", str(args.rate),
                           "--secs", "0", "--thresh", "0.3", "--freq-offset", str(shift)]
                decoded = subprocess.run(command, capture_output=True, text=True, timeout=30)
                name = f"candidate_{index:03d}_{int(delta)}.log"
                (args.output / name).write_text(decoded.stdout + decoded.stderr)
                found = re.findall(r"frame: ([0-9a-f]+)", decoded.stdout)
                for payload in found:
                    packets[payload] = packet_record(payload)
                results.append(dict(candidate=index, shift_hz=shift, returncode=decoded.returncode, log=name,
                                    crc_valid=len(found)))
                if found and retained_clip is None:
                    retained = args.output / "mavic_pro.decoded.cf32"
                    retained.write_bytes(path.read_bytes())
                    Path(str(retained) + ".meta").write_text(f"sample_rate={args.rate}\nformat=cf32\n")
                    retained_clip = dict(path=str(retained), start_sample=begin, samples=len(samples),
                                         sha256=hashlib.sha256(retained.read_bytes()).hexdigest(),
                                         decode_flags=["--freq-offset", str(shift)],
                                         redistribution="local evaluation only; dataset license not yet verified")
            print(f"decoded candidate {index+1}/{min(len(candidates),100)}", flush=True)
    (args.output / "decode_search.json").write_text(json.dumps(results, indent=2) + "\n")
    evidence = dict(id="mavic_pro", dataset="Tampere", model="DJI_mavic_pro_2G", family="O1", path=str(args.input),
                    rate=args.rate, center=None, status="checked", tested_seconds=count/args.rate,
                    unique_payloads=list(packets.values()))
    if retained_clip is not None:
        evidence["retained_clip"] = retained_clip
    (args.output / "results.json").write_text(json.dumps([evidence], indent=2) + "\n")
    (args.output / "decoded_packets.jsonl").write_text("".join(json.dumps(packet) + "\n" for packet in packets.values()))


if __name__ == "__main__":
    main()
