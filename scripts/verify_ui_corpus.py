#!/usr/bin/env python3
"""Verify retained local corpus packets through both CLI and UI workers, without a radio."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    checks = []
    for row in json.loads((args.report / "results.json").read_text()):
        if "retained_clip" not in row:
            continue
        clip = row["retained_clip"]
        path = Path(clip["path"])
        if not path.is_absolute():
            path = repo / path
        digest_ok = hashlib.sha256(path.read_bytes()).hexdigest() == clip["sha256"]
        flags = ["--format", "cf32", "--rate", str(row["rate"])] + clip["decode_flags"]
        decoded = subprocess.run([str(repo / "build/droneid_decode"), str(path), "--secs", "0"] + flags,
                                 capture_output=True, text=True, timeout=60)
        ui = subprocess.run([str(repo / "build/droneid_ui"), "--check", "--replay", str(path)] + flags,
                            capture_output=True, text=True, timeout=60)
        (args.report / (row["id"] + ".ui.log")).write_text(ui.stdout + ui.stderr)
        (args.report / (row["id"] + ".excerpt_decode.log")).write_text(decoded.stdout + decoded.stderr)
        expected = {packet["payload_hex"] for packet in row["unique_payloads"]}
        cli_packets = re.findall(r"frame: ([0-9a-f]+)", decoded.stdout)
        ui_packets = re.findall(r"packet: telemetry=\w+ frame=([0-9a-f]+)", ui.stdout)
        cli_envelopes = re.findall(r"frame: ([0-9a-f]+)\n  envelope: format=(\w+) logical_bytes=\d+ inner_crc16=(\w+)", decoded.stdout)
        ui_envelopes = re.findall(r"packet: telemetry=\w+ frame=([0-9a-f]+) format=(\w+) inner_crc16=(\w+)", ui.stdout)
        passed = digest_ok and decoded.returncode == 0 and ui.returncode == 0 and bool(cli_packets)
        passed = passed and sorted(cli_packets) == sorted(ui_packets) and set(cli_packets) <= expected
        envelope_match = len(cli_envelopes) == len(cli_packets) and sorted(cli_envelopes) == sorted(ui_envelopes)
        passed = passed and envelope_match
        check = dict(id=row["id"], model=row["model"], sha256_matches=digest_ok,
                     cli_packets=len(cli_packets), ui_packets=len(ui_packets), passed=passed,
                     envelopes_match=envelope_match,
                     inner_crc16_valid=sum(checksum == "OK" for _, _, checksum in cli_envelopes),
                     telemetry_parsed=ui.stdout.count("telemetry=parsed"),
                     telemetry_unsupported=ui.stdout.count("telemetry=unsupported"))
        checks.append(check)
        print(json.dumps(check), flush=True)
    (args.report / "ui_verification.json").write_text(json.dumps(checks, indent=2) + "\n")
    return 0 if checks and all(check["passed"] for check in checks) else 1


if __name__ == "__main__":
    raise SystemExit(main())
