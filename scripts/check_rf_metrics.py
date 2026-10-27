#!/usr/bin/env python3
import argparse
import array
import json
import math
from pathlib import Path
import subprocess
import sys


def estimate_noise(path):
    samples = array.array("h")
    with path.open("rb") as capture:
        samples.frombytes(capture.read(4 * 1048576))
    if sys.byteorder != "little":
        samples.byteswap()
    powers = []
    for start in range(0, len(samples) - 63, 64):
        powers.append(sum(value * value for value in samples[start:start + 64]) / (32 * 32768**2))
    if not powers:
        raise ValueError(f"empty capture: {path}")
    powers.sort()
    return 10 * math.log10(max(powers[int(0.2 * (len(powers) - 1))], 1e-20))


def main():
    parser = argparse.ArgumentParser(description="Offline RF metrics smoke check, not O4 validation")
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    for index, path in enumerate(args.files):
        metadata = dict(line.split("=", 1) for line in Path(str(path) + ".meta").read_text().splitlines()
                        if "=" in line)
        metadata = {key.strip(): value.strip().strip('"') for key, value in metadata.items()}
        if metadata.get("format") != "sc16":
            raise ValueError("noise estimator currently supports sc16 only")
        before = path.stat()
        noise = estimate_noise(path)
        command = [str(root / "build/rf_analyze"), str(path), "--noise-dbfs", str(noise)]
        result = {"file": str(path), "command": command, "estimated_noise_dbfs": noise,
                  "noise_method": "20th percentile of 32-sample powers in first 1048576 samples",
                  "events": 0, "truncated_events": 0, "classification": "unknown_rf"}
        with (args.output / f"{index}.jsonl").open("w") as output, (args.output / f"{index}.stderr").open("w") as errors:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors, text=True)
            for line in process.stdout:
                output.write(line)
                event = json.loads(line)
                if event["event"] == "rf_burst":
                    result["events"] += 1
                    result["truncated_events"] += int(event["truncated"])
                else:
                    result["summary"] = event
            if process.wait() != 0:
                raise RuntimeError(f"RF analyzer failed: {path}")
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise RuntimeError(f"capture changed while reading: {path}")
        if result["summary"]["samples"] != before.st_size // 4:
            raise RuntimeError(f"incomplete file check: {path}")
        result["input_bytes"] = before.st_size
        results.append(result)
        print(f"{path.name}: {result['events']} RF events, {result['truncated_events']} truncated, no identity claim", flush=True)
    (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
