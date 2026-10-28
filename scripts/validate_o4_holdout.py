#!/usr/bin/env python3
"""Frozen v2 evaluation on deterministic files/members absent from v1 evaluation.

Not session-disjoint: related records may share sessions. No tuning on these cases.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import subprocess
from validate_corpus import family
from validate_o4_external import evaluate, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('/media/sudeep/Storage/datasets'))
    parser.add_argument('--previous', type=Path, default=Path('data/analysis/o4_external_20261028/manifest.json'))
    parser.add_argument('--binary', type=Path, default=Path('build/capi-linux/o4_detect'))
    parser.add_argument('--output', type=Path, default=Path('data/analysis/o4_v2_holdout_20261028'))
    parser.add_argument('--scratch', type=Path, default=Path('/tmp'))
    args = parser.parse_args()
    args.seconds = .64
    excluded = {c['path'] for c in json.loads(args.previous.read_text())}
    groups = defaultdict(list)
    for path in sorted((args.root/'DRFF-R2/DRFF-R2').glob('dataset*/*.mat')):
        model = path.name.split('_')[0]
        # Same four scenarios as v1, with a different file chosen before looking at IQ.
        if path.parent.name not in ['dataset1-single_drone_states', 'dataset3-single_drone_hover',
                                    'dataset4-single_drone_dual_frequency', 'dataset6-wifi_mixed']:
            continue
        if str(path) not in excluded and family(model) != 'unverified/other':
            groups[path.parent.name, model].append(path)
    cases = []
    for (group, model), paths in sorted(groups.items()):
        path = paths[len(paths)//2]
        cases.append(dict(dataset='DroneRFa', group=group, model=model, family=family(model), kind='hdf', path=str(path)))
    for model in ['DJI AVATA2', 'DJI MINI4 PRO', 'DJI MAVIC3 PRO', 'FRSKY X14']:
        path = args.root/'RFUAV'/(model+'.rar')
        members = [n for n in subprocess.check_output(['unrar','lb',str(path)], text=True).splitlines() if n.endswith('.iq')]
        for index in sorted({len(members)//2, len(members)-1} - {0}):
            cases.append(dict(dataset='RFUAV', group='preselected_middle_last_members', model=model,
                              family=family(model), kind='rar', path=str(path), member=members[index]))
    args.output.mkdir(parents=True, exist_ok=True)
    for i, case in enumerate(cases):
        case['id'] = f'{i:03d}-'+hashlib.sha256((case['path']+case.get('member','')).encode()).hexdigest()[:10]
    (args.output/'manifest.json').write_text(json.dumps(cases,indent=2)+'\n')
    (args.output/'frozen_hashes.json').write_text(json.dumps({p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
        for p in [str(args.binary), 'src/o4/o4_detector.cpp', 'include/droneid/o4_detector.h']},indent=2)+'\n')
    results = []
    for case in cases:
        result = evaluate(case,args)
        results.append(result)
        (args.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(case['id'], case['model'], result.get('summary',result.get('error')),flush=True)
    report(results,args.output,scope='Frozen o4_structure_v2 on preselected files/archive members absent from the development evaluation. 0.64-second prefixes; no tuning on these results. Files may share sessions/devices with development, so this is file-held-out evidence, not session-held-out identity accuracy. Recording labels are not event truth.')
    if not cases or any(r['status']!='ok' for r in results):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
