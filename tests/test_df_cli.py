import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("DRONEID_BUILD_DIR", REPO / "build"))


def checksum(data):
    result = 14695981039346656037
    for value in data:
        result = ((result ^ value) * 1099511628211) & ((1 << 64) - 1)
    return f"{result:016x}"


class PairedCliTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.manifest = self.root / "pair.toml"
        self.calibration = self.root / "cal.toml"
        geometry = 2 * math.pi * 0.05 * 2459.5e6 / 299792458 * math.sin(math.radians(20))
        channels = [bytearray(), bytearray()]
        for index in range(12288):
            amplitude = 0.3 if index < 4096 or 6144 <= index < 8192 else 0
            phase = 2 * math.pi * 150000 * index / 15360000
            for channel in range(2):
                offset = channel * (0.4 + (geometry if index >= 6144 else 0))
                channels[channel] += struct.pack("<ff", amplitude * math.cos(phase + offset),
                                                 amplitude * math.sin(phase + offset))
        for channel, samples in enumerate(channels):
            (self.root / f"ch{channel}.cf32").write_bytes(samples)
        fields = dict(schema_version=1, complete=True, status_reason="synthetic", session_id="test-session",
                      config_epoch=1, rig_id="test-rig", cable_id="synthetic", sample_rate=15360000,
                      center_hz=2459500000, gain_ch0_db=20, gain_ch1_db=20, channel_0=0, channel_1=1,
                      spacing_m=0.05, first_device_timestamp_ns=1000000000,
                      capture_host_unix_ns=1700000000000000000, sample_count=12288, overflows=0,
                      ch0_path="ch0.cf32", ch1_path="ch1.cf32", ch0_fnv1a64=checksum(channels[0]),
                      ch1_fnv1a64=checksum(channels[1]))
        self.manifest.write_text("".join(f"{key} = {json.dumps(value)}\n" for key, value in fields.items()))
        result = self.run_df("--splitter-calibrate", "--samples", "4096", "--spacing-m", "0.05",
                             "--measured-unix-ns", "1700000000000000000")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.calibration.write_text(result.stdout)

    def run_df(self, *arguments):
        return subprocess.run([str(BUILD / "df_bearing"), "--manifest", str(self.manifest), *arguments],
                              capture_output=True, text=True, timeout=30)

    def estimate(self):
        return self.run_df("--cal", str(self.calibration), "--start-sample", "6144", "--samples", "2048")

    def test_cli_and_ui_agree_at_capture_time(self):
        result = self.estimate()
        self.assertEqual(result.returncode, 0, result.stderr)
        cli = json.loads(result.stdout)
        self.assertAlmostEqual(cli["broadside_deg"], 20, places=3)
        events = self.root / "events.jsonl"
        result = subprocess.run([str(BUILD / "droneid_ui"), "--check", "--pair-manifest",
                                 str(self.manifest), "--df-cal", str(self.calibration), "--df-jsonl", str(events)],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        records = [json.loads(line) for line in events.read_text().splitlines()]
        target = [record for record in records if record.get("event") == "df_bearing" and
                  record["start_sample"] == 6144]
        self.assertEqual(len(target), 1)
        self.assertTrue(target[0]["valid"])
        self.assertAlmostEqual(target[0]["broadside_deg"], cli["broadside_deg"], places=3)
        self.assertEqual(records[-1]["state"], "Disconnected")

    def test_legacy_calibration_is_not_manifest_verified(self):
        text = self.calibration.read_text()
        self.calibration.write_text("\n".join(line for line in text.splitlines()
                                             if not line.startswith(("session_id", "config_epoch", "rig_id"))))
        result = self.estimate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("calibration rejected", result.stderr)

    def test_zero_gain_is_not_a_wildcard(self):
        self.calibration.write_text(self.calibration.read_text().replace("gain_db = 20", "gain_db = 0"))
        result = self.estimate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("gain mismatch", result.stderr)

    def test_expiry_advances_with_sample_time(self):
        self.calibration.write_text(self.calibration.read_text().replace("max_age_seconds = 3600", "max_age_seconds = 0.0002"))
        result = self.estimate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expired", result.stderr)
        result = subprocess.run([str(BUILD / "droneid_ui"), "--check", "--pair-manifest",
                                 str(self.manifest), "--df-cal", str(self.calibration),
                                 "--df-jsonl", str(self.root / "expired.jsonl")],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        events = [json.loads(line) for line in (self.root / "expired.jsonl").read_text().splitlines()]
        target = next(event for event in events if event.get("start_sample") == 6144)
        self.assertFalse(target["valid"])
        self.assertIn("expired", target["rejection_reason"])


if __name__ == "__main__":
    unittest.main()
