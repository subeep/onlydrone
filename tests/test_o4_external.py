import json
from pathlib import Path
import sys
import tempfile
import unittest

import h5py

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from validate_o4_external import inventory, report


class ExternalO4EvaluationTests(unittest.TestCase):
    def test_inventory_spreads_o4_and_keeps_environment_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            folder = root / 'DRFF-R2/DRFF-R2/dataset3-single_drone_hover'
            folder.mkdir(parents=True)
            for model in ['mini4PRO', 'mini5PRO', 'mavicAir2s', 'mavic3S']:
                for n in range(5):
                    with h5py.File(folder / f'{model}_{n}.mat', 'w') as h:
                        h['CenterFrequence'] = [[5745e6]]
            environment = root / 'DRFF-R2/DRFF-R2/dataset7-environment'
            environment.mkdir()
            with h5py.File(environment / 'indoor_environment.mat', 'w'):
                pass
            cases = inventory(root)
            self.assertEqual(sum(c['family']=='O4' for c in cases), 3)
            self.assertEqual(sum(c['family']=='O4+' for c in cases), 3)
            self.assertEqual(sum(c['family']=='O3' for c in cases), 1)
            self.assertEqual(sum(c['family']=='unverified/other' for c in cases), 1)
            self.assertEqual(sum(c['family']=='negative-no-drone' for c in cases), 1)
            self.assertEqual({Path(c['path']).name for c in cases if c['family']=='O4'},
                             {'mini4PRO_0.mat', 'mini4PRO_2.mat', 'mini4PRO_4.mat'})
            self.assertEqual(cases, inventory(root))

    def test_report_does_not_promote_weak_labels_to_accuracy(self):
        common = dict(status='ok', tested_seconds=.32, model='test', group='group', center=5745e6)
        rows = [dict(common, id='a', family='O4', summary=dict(candidate_windows=4, windows=20)),
                dict(common, id='b', family='O3+', summary=dict(candidate_windows=2, windows=20)),
                dict(status='error', id='c', model='bad', family='O4', error='truncated')]
        with tempfile.TemporaryDirectory() as directory:
            report(rows, Path(directory))
            metrics = json.loads((Path(directory)/'metrics.json').read_text())
            self.assertIsNone(metrics['identity_accuracy'])
            self.assertEqual(metrics['errors'], 1)
            self.assertEqual(metrics['by_family']['O4']['captures'], 1)
            self.assertEqual(metrics['by_family']['O3+']['captures_with_candidates'], 1)
            self.assertEqual(metrics['by_family']['O3+']['candidate_windows'], 2)


if __name__ == '__main__':
    unittest.main()
