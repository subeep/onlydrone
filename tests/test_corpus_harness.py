import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

import h5py
import numpy as np

spec = importlib.util.spec_from_file_location("corpus", Path(__file__).resolve().parents[1] / "scripts/validate_corpus.py")
corpus = importlib.util.module_from_spec(spec)
spec.loader.exec_module(corpus)


class CorpusHarnessTests(unittest.TestCase):
    def test_family_labels(self):
        for model, expected in [("DJI MINI3", "O2"), ("mini3pro", "O3"), ("mavicAir2s", "O3"),
                                ("mini5PRO", "O4+"), ("mavic3S", "unverified/other"), ("FRSKY X14", "non-DJI")]:
            self.assertEqual(corpus.family(model), expected)

    def test_unsupported_payload_has_no_serial(self):
        payload = bytes.fromhex("8010494e4650") + b"X" * 170
        self.assertIsNone(corpus.packet_record(payload.hex())["serial"])
        self.assertIsNone(corpus.packet_record(payload.hex())["version"])

    def test_hdf_and_zip_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.mat"
            with h5py.File(source, "w") as handle:
                handle["RF0_I"] = np.array([[1], [2], [3]], dtype=np.float32)
                handle["RF0_Q"] = np.array([[-1], [-2], [-3]], dtype=np.float32)
                handle["Fs"] = np.array([[100e6]])
                handle["CenterFrequence"] = np.array([[2440e6]])
            iq, rate, center, _, duration = corpus.load(dict(kind="hdf", path=str(source)), 2e-8, root)
            np.testing.assert_array_equal(iq, np.array([1-1j, 2-2j]))
            self.assertEqual((rate, center, duration), (100e6, 2440e6, 3e-8))
            with h5py.File(source, "w") as handle:
                values = np.zeros((1, 3), dtype=[("real", "<f8"), ("imag", "<f8")])
                values["real"] = [[1, 2, 3]]
                values["imag"] = [[-1, -2, -3]]
                handle["uhd_samps"] = values
            archive = root / "capture.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.write(source, "../unsafe-member.mat")
            iq, rate, center, member, _ = corpus.load(dict(kind="zip_hdf", path=str(archive)), 2e-8, root)
            np.testing.assert_array_equal(iq, np.array([1-1j, 2-2j]))
            self.assertEqual(member, "../unsafe-member.mat")
            self.assertTrue((root / "member.mat").exists())

    def test_envelope_boundaries_and_crc(self):
        for payload in (b"", bytes([88]), bytes([0, 0x10, 2]), bytes([171]) + bytes(175)):
            self.assertEqual(corpus.packet_envelope(payload)["inner_crc16"], "invalid_length")
        for length, header, expected in [(79, b"\x10\x02", "v2"), (91, b"\x10\x02", "v2"),
                                         (112, b"\x10\x03", "v3"), (131, b"\x10INFP", "infp"),
                                         (138, b"\x10INFP", "infp"), (144, b"\x11\x02", "auxiliary_11"),
                                         (173, b"\x13CRYP", "cryp")]:
            payload = bytearray(176)
            payload[0] = length - 3
            payload[1:1 + len(header)] = header
            payload[7:11] = b"TEST"
            checksum = 0x3692
            for value in payload[:length - 2]:
                checksum ^= value
                for _ in range(8):
                    checksum = (checksum >> 1) ^ (0x8408 if checksum & 1 else 0)
            payload[length - 2:length] = checksum.to_bytes(2, "little")
            record = corpus.packet_record(payload.hex())
            self.assertEqual(record["inner_crc16"], "OK")
            self.assertEqual(record["packet_format"], expected)
            self.assertEqual(record["serial"], "TEST" if expected == "v2" else None)
            payload[7] ^= 1
            self.assertEqual(corpus.packet_record(payload.hex())["inner_crc16"], "fail")
            self.assertIsNone(corpus.packet_record(payload.hex())["serial"])

    def test_int16_scaling(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "samples.bin"
            np.array([32767, -32768, 16384, -16384], dtype="<i2").tofile(path)
            iq, *_ = corpus.load(dict(kind="sc16", path=str(path), rate=120e6, center=None), 1, Path(directory))
            np.testing.assert_allclose(iq, [32767/32768-1j, 0.5-0.5j])

    def test_activity_is_not_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = [dict(dataset="test", model=family, family=family, status="checked", rf_events=1,
                         unique_payloads=[]) for family in ["O4", "O4+", "O2", "non-DJI", "negative-no-drone"]]
            corpus.report(rows, Path(directory))
            metrics = json.loads((Path(directory) / "detection_metrics.json").read_text())
            self.assertIsNone(metrics["o4_classifier_accuracy"])
            self.assertEqual(metrics["o4_labeled_windows"], 1)
            self.assertEqual(metrics["o4_plus_windows_excluded"], 1)
            self.assertEqual(metrics["activity_false_positive_rate"], 1)


if __name__ == "__main__":
    unittest.main()
