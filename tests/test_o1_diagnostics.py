import importlib.util
from pathlib import Path
import sys
import unittest

import numpy as np

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "scripts"))
spec = importlib.util.spec_from_file_location("o1_diagnostics", root / "scripts/investigate_o1.py")
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)


class O1DiagnosticsTests(unittest.TestCase):
    def test_silence_is_not_a_candidate(self):
        samples = np.zeros(32768, dtype=np.complex64)
        found, overview = diagnostics.regions(samples, 120e6, 0)
        self.assertEqual(found, [])
        self.assertEqual(overview.shape, (2, 1024))
        self.assertTrue(np.isfinite(overview).all())

    def test_known_wideband_fixture_region_and_time_offset(self):
        samples = np.fromfile(root / "tests/data/mini2_wideband.cf32", dtype="<c8")
        found, _ = diagnostics.regions(samples, 50e6, 0)
        shifted, _ = diagnostics.regions(samples, 50e6, 0.5)
        self.assertGreater(len(found), 0)
        self.assertEqual(len(found), len(shifted))
        for first, second in zip(found, shifted):
            self.assertAlmostEqual(first["start_seconds"] + 0.5, second["start_seconds"])
            self.assertEqual(first["bandwidth_hz"], second["bandwidth_hz"])
            self.assertEqual(first["center_offset_hz"], second["center_offset_hz"])


if __name__ == "__main__":
    unittest.main()
