import argparse
import concurrent.futures
import datetime
import json
import pathlib
import re
import subprocess


def metadata_for(path):
    metadata = {}
    for line in pathlib.Path(str(path) + ".meta").read_text().splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            metadata[key.strip()] = value.strip().strip('"')
    return metadata


def inspect_capture(path, root, decoder, output):
    relative = path.relative_to(root).as_posix()
    metadata = metadata_for(path)
    initial = path.stat()
    rate = float(metadata["sample_rate"])
    center = float(metadata["center_freq"])
    sample_bytes = {"sc16": 4, "cf32": 8, "sc8": 2}[metadata["format"]]
    channels = [2399.5e6, 2414.5e6, 2429.5e6, 2444.5e6, 2459.5e6,
                5756.5e6, 5776.5e6, 5796.5e6]
    shifts = sorted({center - channel for channel in channels
                     if 0 < abs(center - channel) < rate / 2})
    modes = [("centered", []), ("auto", ["--auto-center"])]
    modes.extend((f"shift_{shift:+.0f}", ["--freq-offset", str(shift)]) for shift in shifts)
    modes.append(("scan_prefix", ["--scan"]))
    result = {
        "path": relative, "bytes": initial.st_size, "metadata": metadata,
        "seconds": initial.st_size / sample_bytes / rate,
        "partial_sample_bytes": initial.st_size % sample_bytes,
        "fully_contained_known_channels_hz": [channel for channel in channels
            if abs(channel - center) + 5e6 <= rate / 2],
        "runs": [],
    }
    for name, options in modes:
        command = [str(decoder), str(path), "--secs", "0", "--thresh", "0.3"] + options
        completed = subprocess.run(command, capture_output=True, text=True, timeout=600)
        text = completed.stderr + completed.stdout
        log = output / "logs" / relative / (name + ".log")
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(text)
        run = {"mode": name, "command": command, "returncode": completed.returncode,
               "log": log.relative_to(output).as_posix()}
        summary = re.search(r"== (\d+) burst candidate\(s\), (\d+) CRC-valid ==", text)
        if summary:
            run.update(candidates=int(summary[1]), crc_valid=int(summary[2]))
        if name == "scan_prefix":
            scores = re.findall(r"^\s*([+-]?\d+\.\d+)\s+(\d+\.\d+)\s+(\d+)\s*$",
                                completed.stdout, re.MULTILINE)
            if scores:
                peak = max(scores, key=lambda score: float(score[1]))
                run.update(peak_corr=float(peak[1]), peak_shift_mhz=float(peak[0]),
                           peak_sample=int(peak[2]), scanned_samples=min(initial.st_size // sample_bytes, 1048576))
        chosen_shift = re.search(r"shift ([+-]\d+) Hz", completed.stderr)
        if chosen_shift:
            run["applied_shift_hz"] = int(chosen_shift[1])
        run["success"] = completed.returncode == 0 and (summary is not None or "peak_corr" in run)
        result["runs"].append(run)
    final = path.stat()
    result["unchanged_during_check"] = (initial.st_size, initial.st_mtime_ns) == (final.st_size, final.st_mtime_ns)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", type=pathlib.Path, default=pathlib.Path("data"))
    parser.add_argument("--decoder", type=pathlib.Path, default=pathlib.Path("build/droneid_decode"))
    parser.add_argument("--output", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    root, decoder, output = arguments.data.resolve(), arguments.decoder.resolve(), arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    paths = sorted(path for path in root.rglob("*.iq") if path.is_file())
    results = []
    started = datetime.datetime.now(datetime.timezone.utc).isoformat()
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = {executor.submit(inspect_capture, path, root, decoder, output): path for path in paths}
        for future in concurrent.futures.as_completed(futures):
            path = futures[future]
            try:
                result = future.result()
            except Exception as error:
                result = {"path": path.relative_to(root).as_posix(), "error": str(error)}
            results.append(result)
            valid = sum(run.get("crc_valid", 0) for run in result.get("runs", []))
            print(f"{len(results)}/{len(paths)} {result['path']}: CRC-valid across passes={valid}"
                  + (f" ERROR: {result['error']}" if "error" in result else ""), flush=True)
            (output / "results.json").write_text(json.dumps({"started_utc": started,
                "captures": sorted(results, key=lambda item: item["path"])}, indent=2) + "\n")
    failed = any("error" in result or not result.get("unchanged_during_check", False) or
                 not all(run["success"] for run in result.get("runs", [])) for result in results)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
