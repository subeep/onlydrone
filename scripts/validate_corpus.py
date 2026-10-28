#!/usr/bin/env python3
"""Bounded, read-only local IQ validation. Does not download or modify datasets."""
import argparse
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET
import zipfile

try:
    import h5py
except ImportError:
    h5py = None
import numpy as np


FAMILIES = {"mavicair2": "O2", "mavicair2s": "O3", "mini3pro": "O3",
            "mavic3": "O3+", "mavic3c": "O3+", "mini4pro": "O4", "mini5pro": "O4+"}


def family(model):
    compact = re.sub(r"[^a-z0-9]", "", model.lower())
    for token, generation in [("avata2", "O4"), ("mini4pro", "O4"), ("mini5pro", "O4+"),
                               ("air3s", "O4"), ("mavic3pro", "O3+"), ("mini3pro", "O3"),
                               ("mini3", "O2"), ("mini2", "O2"), ("mavicair2s", "O3"),
                               ("mavicair2", "O2"), ("mavicpro", "O1"),
                               ("frsky", "non-DJI"), ("futaba", "non-DJI"), ("herelink", "non-DJI"),
                               ("parrot", "non-DJI"), ("yuneec", "non-DJI"), ("sjrc", "non-DJI")]:
        if token in compact:
            return generation
    return FAMILIES.get(compact, "unverified/other")


def scalar(handle, *names):
    for name in names:
        if name in handle:
            return float(handle[name][()].ravel()[0])
    raise ValueError(f"missing metadata field: {names}")


def packet_envelope(raw):
    length = raw[0] + 3 if raw else 0
    result = dict(packet_format="unknown", logical_bytes=length, inner_crc16="invalid_length")
    if not 5 <= length <= min(len(raw), 173):
        return result
    checksum = 0x3692
    for value in raw[:length]:
        checksum ^= value
        for _ in range(8):
            checksum = (checksum >> 1) ^ (0x8408 if checksum & 1 else 0)
    result["inner_crc16"] = "OK" if checksum == 0 else "fail"
    if raw[1] == 0x10 and raw[2] in (2, 3):
        result["packet_format"] = f"v{raw[2]}"
    elif length >= 8 and raw[1:6] == b"\x10INFP":
        result["packet_format"] = "infp"
    elif length >= 8 and raw[1:6] == b"\x13CRYP":
        result["packet_format"] = "cryp"
    elif raw[1] == 0x11:
        result["packet_format"] = "auxiliary_11"
    return result


def packet_record(payload):
    raw = bytes.fromhex(payload)
    envelope = packet_envelope(raw)
    serial_bytes = raw[7:23].split(b"\x00")[0]
    parsed = (envelope["packet_format"] == "v2" and envelope["inner_crc16"] == "OK" and
              envelope["logical_bytes"] in (79, 91) and bool(serial_bytes) and
              all(32 <= value <= 126 for value in serial_bytes))
    return dict(payload_hex=payload, serial=serial_bytes.decode("ascii") if parsed else None,
                version=raw[2] if parsed else None, telemetry_status="v2_serial_parsed" if parsed else "unsupported_payload",
                header_hex=raw[:6].hex(), **envelope,
                validation="outer CRC-24 checked by decoder; inner CRC-16 checked here; neither authenticates identity; no dataset telemetry ground truth")


def inventory(root, repo, per_group):
    all_files = [{"path": str(path), "bytes": path.stat().st_size}
                 for path in sorted(root.rglob("*")) if path.is_file() and
                 path.suffix.lower() in {".mat", ".bin", ".rar", ".zip", ".iq"}]
    cases = []
    groups = defaultdict(list)
    for path in sorted((root / "DRFF-R2").rglob("*.mat")):
        if path.parent.name not in {"dataset3-single_drone_hover", "dataset4-single_drone_dual_frequency"}:
            continue
        if h5py is None:
            raise RuntimeError("h5py is required to inventory HDF5 .mat files")
        with h5py.File(path) as handle:
            center = scalar(handle, "CenterFrequence", "CenterFreq")
        model = path.name.split("_")[0]
        groups[(model, round(center / 1e8))].append(path)
    for (model, _), paths in groups.items():
        for path in paths[:per_group]:
            cases.append(dict(dataset="DroneRFa", model=model, family=FAMILIES.get(model.lower(), "unverified/other"),
                              kind="hdf", path=str(path)))
    for path in sorted((root / "RFUAV").glob("*.rar")):
        if not path.name.startswith("DJI") and path.stem not in {"FRSKY X14", "FUTABA T18SZ", "Herelink Hx4"}:
            continue
        cases.append(dict(dataset="RFUAV", model=path.stem, family=family(path.stem), kind="rar", path=str(path)))
    for path in sorted((root / "KULeuven_DroneRF").glob("*.zip")):
        if path.stem.endswith("2") and path.stem.startswith("Mavic"):
            continue
        cases.append(dict(dataset="KULeuven", model=path.stem, family=family(path.stem), kind="zip_hdf", path=str(path)))
    for path in sorted((root / "Tampere_DroneRF").glob("*.bin")):
        if "2of2" in path.name:
            continue
        cases.append(dict(dataset="Tampere", model=path.stem, family=family(path.stem), kind="sc16",
                          rate=200e6 if "_5G" in path.name else 120e6, center=None, path=str(path)))
    for path in sorted((repo / "data/iq_samples").rglob("*.cf32")):
        cases.append(dict(dataset="RUB-reference", model=path.parent.name, family="O2", kind="cf32",
                          rate=50e6, center=None, path=str(path)))
    for folder, generation in [("baseline", "negative-no-drone"), ("air3s", "O4"), ("mini2", "O2")]:
        for path in sorted((repo / "data" / folder).rglob("*.iq")):
            if not path.is_file():
                continue
            metadata = dict(line.split("=", 1) for line in Path(str(path) + ".meta").read_text().splitlines() if "=" in line)
            metadata = {key.strip(): value.strip().strip('"') for key, value in metadata.items()}
            cases.append(dict(dataset="Local", model=folder, family=generation, kind="sc16",
                              rate=float(metadata["sample_rate"]), center=float(metadata["center_freq"]), path=str(path)))
    for index, case in enumerate(cases):
        case["id"] = f"{index:03d}-" + hashlib.sha256(case["path"].encode()).hexdigest()[:10]
        case["family_provenance"] = "dataset model label + docs/MODEL_FAMILIES.md; not a PHY identification"
    return all_files, cases


def load(case, seconds, temporary):
    path = Path(case["path"])
    kind = case["kind"]
    if kind == "zip_hdf":
        if h5py is None:
            raise RuntimeError("h5py is required to load zip_hdf cases")
        with zipfile.ZipFile(path) as archive:
            entries = sorted((entry for entry in archive.infolist() if entry.filename.endswith(".mat")), key=lambda entry: entry.filename)
            member = entries[0]
            if member.file_size > 512 * 1024**2:
                raise ValueError("first MAT member exceeds the 512 MiB extraction cap")
            extracted = temporary / "member.mat"
            with archive.open(member) as source, extracted.open("wb") as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
        with h5py.File(extracted) as handle:
            data = handle["uhd_samps"]
            count = min(int(100e6 * seconds), data.size)
            values = data[0, :count] if data.shape[0] == 1 else data[:count, 0]
            iq = (values["real"] + 1j * values["imag"]).astype("<c8")
            return iq, 100e6, 2.44e9, member.filename, data.size / 100e6
    if kind == "hdf":
        if h5py is None:
            raise RuntimeError("h5py is required to load hdf cases")
        with h5py.File(path) as handle:
            rate = scalar(handle, "Fs")
            center = scalar(handle, "CenterFrequence", "CenterFreq")
            count = min(int(rate * seconds), handle["RF0_I"].shape[0])
            iq = (handle["RF0_I"][:count, 0] + 1j * handle["RF0_Q"][:count, 0]).astype("<c8")
            return iq, rate, center, None, handle["RF0_I"].shape[0] / rate
    if kind == "rar":
        listing = subprocess.check_output(["unrar", "lb", str(path)], text=True, timeout=30).splitlines()
        member = case.get("member") or next(name for name in listing if name.endswith(".iq"))
        if member not in listing or not member.endswith(".iq"):
            raise ValueError("requested IQ archive member is absent")
        parent = str(Path(member).parent)
        xml = next(name for name in listing if name.endswith(".xml") and str(Path(name).parent) == parent)
        metadata = ET.fromstring(subprocess.check_output(["unrar", "p", "-inul", str(path), xml], timeout=30))
        if metadata.findtext("DataType") != "Complex Float":
            raise ValueError("unsupported RFUAV sample type")
        rate = float(metadata.findtext("SampleRate"))
        center = float(metadata.findtext("CenterFrequency"))
        process = subprocess.Popen(["unrar", "p", "-inul", str(path), member], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            raw = process.stdout.read(int(rate * seconds) * 8)
        finally:
            process.stdout.close()
            process.terminate()
            process.wait(timeout=15)
        iq = np.frombuffer(raw, dtype="<c8").copy()
        return iq, rate, center, member, None
    rate, center = case["rate"], case["center"]
    count = int(seconds * rate)
    if kind == "sc16":
        raw = np.fromfile(path, dtype="<i2", count=count * 2)
        iq = (raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)) / 32768
    else:
        iq = np.fromfile(path, dtype="<c8", count=count)
    return iq.astype("<c8"), rate, center, None, path.stat().st_size / ((4 if kind == "sc16" else 8) * rate)


def run_case(case, args):
    result = dict(case)
    start = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix="corpus-", dir=args.scratch) as directory:
            temporary = Path(directory)
            iq, rate, center, member, total_seconds = load(case, args.seconds, temporary)
            if iq.size < 8192 or not np.isfinite(iq).all():
                raise ValueError("too few or nonfinite IQ samples")
            result.update(rate=rate, center=center, member=member, tested_samples=int(iq.size),
                          tested_seconds=iq.size / rate, source_seconds=total_seconds,
                          clip_sha256=hashlib.sha256(iq.tobytes()).hexdigest())
            clip = temporary / "clip.cf32"
            iq.tofile(clip)
            envelope = np.mean(np.abs(iq[:iq.size // 32 * 32].reshape(-1, 32))**2, axis=1)
            noise = float(10 * np.log10(max(float(np.percentile(envelope, 20)), 1e-20)))
            noise = min(0.0, max(-200.0, noise))
            rf = subprocess.run([str(args.repo / "build/rf_analyze"), str(clip), "--format", "cf32",
                "--rate", str(rate), "--noise-dbfs", str(noise)], capture_output=True, text=True, timeout=120)
            if rf.returncode:
                raise RuntimeError(rf.stderr)
            events = [json.loads(line) for line in rf.stdout.splitlines()]
            result.update(rf_events=events[-1]["rf_events"], noise_dbfs=noise, o4_identification="abstain_unknown_rf")
            (args.output / (case["id"] + ".rf.jsonl")).write_text(rf.stdout)
            modes = [("auto", ["--auto-center"])]
            if center:
                channels = [2399.5e6, 2414.5e6, 2429.5e6, 2444.5e6, 2459.5e6, 5756.5e6, 5776.5e6, 5796.5e6]
                modes += [(f"channel_{int(channel)}", ["--freq-offset", str(center - channel)]) for channel in channels
                          if abs(channel - center) + 5e6 <= rate / 2]
            modes += [(f"extra_offset_{shift:g}", ["--freq-offset", str(shift)]) for shift in args.extra_offset]
            result["passes"] = []
            packets = {}
            for mode, flags in modes:
                command = [str(args.repo / "build/droneid_decode"), str(clip), "--format", "cf32", "--rate", str(rate),
                           "--secs", "0", "--thresh", "0.3"] + flags
                decoded = subprocess.run(command, capture_output=True, text=True, timeout=180)
                (args.output / (case["id"] + "." + mode + ".log")).write_text(decoded.stdout + decoded.stderr)
                if decoded.returncode:
                    raise RuntimeError(f"decoder {mode} failed: {decoded.stderr[-300:]}")
                summary = re.search(r"== (\d+) burst candidate\(s\), (\d+) CRC-valid", decoded.stdout)
                if not summary:
                    raise RuntimeError("missing decoder summary")
                result["passes"].append(dict(mode=mode, candidates=int(summary[1]), crc_valid=int(summary[2])))
                for payload in re.findall(r"frame: ([0-9a-f]+)", decoded.stdout):
                    packets[payload] = packet_record(payload)
                match = re.search(r"idx=(\d+)[^\n]*crc=OK", decoded.stdout)
                if match and "retained_clip" not in result and case["dataset"] != "RUB-reference":
                    begin = max(0, int(match[1]) - int(rate * 0.0002))
                    excerpt = iq[begin:begin + int(rate * 0.002)]
                    retained = args.output / (case["id"] + ".decoded.cf32")
                    excerpt.tofile(retained)
                    Path(str(retained) + ".meta").write_text(f"format=cf32\nsample_rate={rate}\ncenter_freq={center or 0}\n")
                    result["retained_clip"] = dict(path=str(retained), start_sample=begin, samples=int(excerpt.size),
                        sha256=hashlib.sha256(excerpt.tobytes()).hexdigest(), decode_flags=flags,
                        redistribution="local evaluation only; dataset license not yet verified")
            result["unique_payloads"] = list(packets.values())
            result["status"] = "checked"
    except Exception as error:
        result.update(status="error", error=str(error))
    result["elapsed_seconds"] = round(time.monotonic() - start, 3)
    return result


def report(results, output):
    groups = defaultdict(list)
    for result in results:
        groups[(result["dataset"], result["model"], result["family"])].append(result)
    lines = ["# Local corpus validation", "", "All windows are bounded prefixes, not whole-archive or whole-dataset coverage.",
             "Model/family names are dataset/operator labels, not independently identified PHY generations.", "",
             "| Dataset | Model | Family label | Windows checked | CRC-valid unique payloads | V2 serial parsed | RF-active windows | Errors |",
             "|---|---|---|---:|---:|---:|---:|---:|"]
    for (dataset, model, generation), rows in sorted(groups.items()):
        good = [row for row in rows if row["status"] == "checked"]
        payloads = {packet["payload_hex"] for row in good for packet in row["unique_payloads"]}
        parsed = sum(packet_record(payload)["serial"] is not None for payload in payloads)
        lines.append(f"| {dataset} | {model} | {generation} | {len(good)} | {len(payloads)} | {parsed} | {sum(row['rf_events'] > 0 for row in good)} | {len(rows)-len(good)} |")
    good = [row for row in results if row["status"] == "checked"]
    positives = [row for row in good if row["family"] == "O4"]
    negatives = [row for row in good if row["family"] in {"negative-no-drone", "non-DJI"} or
                 row["family"] in {"O1", "O2", "O3", "O3+"} and row["dataset"] != "RUB-reference"]
    tp = sum(row["rf_events"] > 0 for row in positives)
    fp = sum(row["rf_events"] > 0 for row in negatives)
    metrics = dict(o4_labeled_windows=len(positives), non_o4_labeled_windows=len(negatives),
                   o4_plus_windows_excluded=sum(row["family"] == "O4+" for row in good),
                   activity_true_positive=tp, activity_false_negative=len(positives)-tp,
                   activity_false_positive=fp, activity_true_negative=len(negatives)-fp,
                   activity_recall=tp / len(positives) if positives else None,
                   activity_false_positive_rate=fp / len(negatives) if negatives else None,
                   activity_precision=tp / (tp+fp) if tp+fp else None,
                   o4_classifier_accuracy=None, reason="No O4 classifier: all identities abstain. RF activity is not O4 detection.")
    lines += ["", "## O4 accuracy limitation", "", "The current backend emits `unknown_rf`, not O4 identities. Its O4 identification accuracy is **not established**.",
              "The following confusion counts deliberately test the incorrect proxy `any RF activity => O4` against explicit model labels.",
              "They quantify why the energy detector is insufficient; they are not an O4 classifier accuracy claim.",
              "No model was trained/tuned on this evaluation and no per-burst ground truth is available.", "", "```json", json.dumps(metrics, indent=2), "```",
              "", "Decode success means outer CRC-24 passed. Missing CRCs do not prove encryption, incompatibility, or drone absence.",
              "Control/video RF captures may contain no DroneID broadcast. Check individual logs and sampled durations before interpreting failures."]
    (output / "REPORT.md").write_text("\n".join(lines) + "\n")
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    (output / "detection_metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    packets = [dict(dataset=row["dataset"], model=row["model"], family=row["family"], source=row["path"],
                    case_id=row["id"], **packet_record(packet["payload_hex"]))
               for row in good for packet in row["unique_payloads"]]
    (output / "decoded_packets.jsonl").write_text("".join(json.dumps(packet) + "\n" for packet in packets))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("/media/sudeep/Storage/datasets"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=0.1)
    parser.add_argument("--per-group", type=int, default=1)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--scratch", type=Path, default=Path("/tmp"))
    parser.add_argument("--datasets", nargs="*")
    parser.add_argument("--model-filter", default="")
    parser.add_argument("--extra-offset", action="append", type=float, default=[])
    parser.add_argument("--refresh-report", action="store_true")
    args = parser.parse_args()
    if not 0 < args.seconds <= 2 or not 1 <= args.jobs <= 4 or not 1 <= args.per_group <= 10:
        parser.error("seconds in (0,2], jobs in [1,4], per-group in [1,10] required")
    if any(not math.isfinite(shift) for shift in args.extra_offset):
        parser.error("extra offsets must be finite")
    args.repo = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=True)
    if args.refresh_report:
        results = json.loads((args.output / "results.json").read_text())
        manifest = json.loads((args.output / "manifest.json").read_text())
        for row in results + manifest:
            if row["dataset"] not in {"Local", "RUB-reference"}:
                row["family"] = family(row["model"])
            row["family_provenance"] = "dataset model label + docs/MODEL_FAMILIES.md; not a PHY identification"
            if "unique_payloads" in row:
                row["unique_payloads"] = [packet_record(packet["payload_hex"]) for packet in row["unique_payloads"]]
        (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        report(results, args.output)
        return
    if not args.root.is_dir():
        parser.error("dataset root is unavailable; mount the drive or select --root")
    all_files, cases = inventory(args.root, args.repo, args.per_group)
    if args.datasets:
        cases = [case for case in cases if case["dataset"] in args.datasets]
    if args.model_filter:
        cases = [case for case in cases if args.model_filter.casefold() in case["model"].casefold()]
    if not cases:
        parser.error("no cases matched the selected dataset/model filters")
    (args.output / "inventory.json").write_text(json.dumps(all_files, indent=2) + "\n")
    (args.output / "manifest.json").write_text(json.dumps(cases, indent=2) + "\n")
    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [executor.submit(run_case, case, args) for case in cases]
        for future in as_completed(futures):
            result = future.result()
            results.append(result)
            report(sorted(results, key=lambda row: row["id"]), args.output)
            print(f"{len(results)}/{len(cases)} {result['dataset']} {result['model']}: {result['status']} "
                  f"CRC payloads={len(result.get('unique_payloads', []))} RF={result.get('rf_events', '?')} {result.get('error', '')}", flush=True)


if __name__ == "__main__":
    main()
