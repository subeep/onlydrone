#!/usr/bin/env python3
"""Bounded frozen-profile evaluation on external recordings, without downloading IQ."""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import time

import h5py
import numpy as np
from validate_corpus import family, load, scalar


def inventory(root):
    cases = []
    base = root / 'DRFF-R2/DRFF-R2'
    for folder in ['dataset1-single_drone_states', 'dataset3-single_drone_hover',
                   'dataset4-single_drone_dual_frequency', 'dataset6-wifi_mixed']:
        groups = defaultdict(list)
        for path in sorted((base / folder).glob('*.mat')):
            model = path.name.split('_')[0]
            with h5py.File(path) as h:
                band = round(scalar(h, 'CenterFrequence', 'CenterFreq') / 1e8)
            groups[model, band].append(path)
        for (model, band), paths in sorted(groups.items()):
            indices = sorted({0, len(paths)//2, len(paths)-1}) if family(model) in ['O4', 'O4+'] else [0]
            for i in indices:
                cases.append(dict(dataset='DroneRFa', group=folder, model=model,
                                  family=family(model), kind='hdf', path=str(paths[i])))
    for path in sorted((base / 'dataset7-environment').glob('*.mat')):
        cases.append(dict(dataset='DroneRFa', group='environment', model=path.stem,
                          family='negative-no-drone', kind='hdf', path=str(path)))
    for name in ['DJI AVATA2', 'DJI MINI4 PRO', 'DJI MAVIC3 PRO', 'DJI MINI3',
                 'DJI FPV COMBO', 'FRSKY X14', 'FUTABA T18SZ', 'Herelink Hx4',
                 'DAUTEL EVO NANO', 'RadioMaster BOXER']:
        path = root / 'RFUAV' / (name+'.rar')
        if path.exists():
            cases.append(dict(dataset='RFUAV', group='first_iq_member', model=name,
                              family=family(name), kind='rar', path=str(path)))
    for i, case in enumerate(cases):
        case['id'] = f'{i:03d}-' + hashlib.sha256(case['path'].encode()).hexdigest()[:10]
    return cases


def evaluate(case, args):
    result = dict(case)
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix='o4-external-', dir=args.scratch) as name:
            scratch = Path(name)
            iq, rate, center, member, source_seconds = load(case, args.seconds, scratch)
            if iq.size < rate*.08 or not np.isfinite(iq).all():
                raise ValueError('insufficient finite data for five 16 ms windows')
            result.update(rate=rate, center=center, member=member, source_seconds=source_seconds,
                          start_sample=0, samples=int(iq.size), tested_seconds=iq.size/rate,
                          clip_sha256=hashlib.sha256(iq.tobytes()).hexdigest(),
                          scaling='unchanged adapter IQ; no per-capture normalization or channel selection',
                          raw_clipped_fraction=float(np.mean((abs(iq.real)>=.999)|(abs(iq.imag)>=.999))))
            path = scratch / 'prefix.cf32'
            iq.tofile(path)
            del iq
            command = [str(args.binary.resolve()), str(path), '--format', 'cf32', '--rate', str(rate)]
            result['command'] = command
            log = args.output / (case['id']+'.jsonl')
            error = args.output / (case['id']+'.stderr')
            with log.open('w') as out, error.open('w') as err:
                process = subprocess.run(command, stdout=out, stderr=err, timeout=180)
            if process.returncode:
                raise RuntimeError(f'detector exit {process.returncode}; see {error}')
            events = [json.loads(line) for line in log.read_text().splitlines()]
            result['summary'] = events[-1]
            result['reasons'] = dict((reason, sum(e.get('reason') == reason for e in events[:-1]))
                                     for reason in sorted(set(e['reason'] for e in events[:-1])))
            result['status'] = 'ok'
    except Exception as exc:
        result.update(status='error', error=str(exc))
    result['wall_seconds'] = time.monotonic()-started
    return result


def report(results, output, scope=None):
    groups = defaultdict(list)
    for r in results:
        if r['status'] == 'ok':
            groups[r['family']].append(r)
    rows = {}
    for label, items in sorted(groups.items()):
        rows[label] = dict(captures=len(items), seconds=sum(r['tested_seconds'] for r in items),
                           captures_with_candidates=sum(r['summary']['candidate_windows']>0 for r in items),
                           candidate_windows=sum(r['summary']['candidate_windows'] for r in items),
                           windows=sum(r['summary']['windows'] for r in items))
    profiles = sorted({r['summary'].get('profile', 'unknown') for r in results if r['status']=='ok'})
    metrics = dict(profile=profiles[0] if len(profiles)==1 else profiles, cases=len(results),
                   errors=sum(r['status']!='ok' for r in results), by_family=rows,
                   scope=scope or 'Bounded prefix evaluation. These recordings were inspected during development of v2, so v2 results here are regression evidence, not held-out accuracy. Recording labels are not event truth; related files may share sessions/devices.',
                   identity_accuracy=None)
    (output/'metrics.json').write_text(json.dumps(metrics,indent=2)+'\n')
    lines=['# Frozen O4 candidate profile: external dataset evaluation','',metrics['scope'],'',
           '| Family label | Captures | With candidates | Candidate / total windows | Seconds |',
           '|---|---:|---:|---:|---:|']
    for family_name,r in rows.items():
        lines.append(f'| {family_name} | {r["captures"]} | {r["captures_with_candidates"]} | {r["candidate_windows"]} / {r["windows"]} | {r["seconds"]:.3f} |')
    lines += ['',f'Errors: {metrics["errors"]}. No IQ downloaded or source files modified.', '',
              'No sensitivity/precision claim from recording-level labels. No per-file normalization or favorable subband selection.', '',
              '| Case | Model | Group | Center MHz | Candidate / total windows |',
              '|---|---|---|---:|---:|']
    for r in results:
        if r['status']=='ok':
            lines.append(f'| {r["id"]} | {r["model"]} | {r["group"]} | {r["center"]/1e6:.1f} | {r["summary"]["candidate_windows"]} / {r["summary"]["windows"]} |')
        else:
            lines.append(f'| {r["id"]} | {r["model"]} | ERROR: {r["error"]} | - | - |')
    (output/'REPORT.md').write_text('\n'.join(lines)+'\n')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path('/media/sudeep/Storage/datasets'))
    parser.add_argument('--output',type=Path,default=Path('data/analysis/o4_external_20261028'))
    parser.add_argument('--binary',type=Path,default=Path('build/o4_detect'))
    parser.add_argument('--scratch',type=Path,default=Path('/tmp'))
    parser.add_argument('--seconds',type=float,default=.32)
    args=parser.parse_args()
    if not .08 <= args.seconds <= 1.4:
        parser.error('seconds must be in [.08,1.4]')
    if not args.root.is_dir():
        parser.error('dataset root is not mounted')
    args.output.mkdir(parents=True,exist_ok=True)
    cases=inventory(args.root)
    if not cases:
        parser.error('no compatible cases found')
    (args.output/'manifest.json').write_text(json.dumps(cases,indent=2)+'\n')
    (args.output/'detector_source_sha256.txt').write_text(hashlib.sha256(Path('src/o4/o4_detector.cpp').read_bytes()).hexdigest()+'\n')
    (args.output/'detector_binary_sha256.txt').write_text(hashlib.sha256(args.binary.read_bytes()).hexdigest()+'\n')
    results=[]
    print(f'Evaluating {len(cases)} bounded prefixes',flush=True)
    for case in cases:
        r=evaluate(case,args)
        results.append(r)
        (args.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(case['id'],case['model'],case['group'],r.get('summary',r.get('error')),flush=True)
    report(results,args.output)
    if any(r['status']!='ok' for r in results):
        raise SystemExit(1)


if __name__=='__main__':
    main()
