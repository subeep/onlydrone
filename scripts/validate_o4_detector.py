#!/usr/bin/env python3
"""Read-only local-recording evaluation; never equates window labels with O4 identity."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def evaluate(path, label, executable, output):
    identifier = path.stem + '-' + hashlib.sha256(str(path).encode()).hexdigest()[:8]
    log = output / (identifier + '.jsonl')
    error = output / (identifier + '.stderr')
    command = [str(executable.resolve()), str(path)]
    started = time.monotonic()
    with log.open('w') as out, error.open('w') as err:
        run = subprocess.run(command, stdout=out, stderr=err, timeout=600)
    if run.returncode:
        raise RuntimeError(f'{path}: exit {run.returncode}; see {error}')
    summary = None
    development = later = 0
    failures = {}
    meta = dict(line.replace(' ', '').split('=', 1) for line in Path(str(path)+'.meta').read_text().splitlines() if '=' in line)
    rate = float(meta['sample_rate'])
    with log.open() as f:
        for line in f:
            row = json.loads(line)
            if row['event'] == 'summary':
                summary = row
            else:
                if row['candidate']:
                    if row['start_sample'] < .32*rate:
                        development += 1
                    else:
                        later += 1
                failures[row['reason']] = failures.get(row['reason'], 0) + 1
    if summary is None:
        raise RuntimeError(f'No summary for {path}')
    return dict(path=str(path), label=label, label_scope='recording, not individual RF events',
                command=command, input_bytes=path.stat().st_size, metadata=meta,
                sample_rate=rate, duration_seconds=summary['samples']/rate,
                wall_seconds=time.monotonic()-started, summary=summary,
                development_candidate_windows=development, later_candidate_windows=later,
                reasons=failures, log=str(log), stderr=str(error))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('data/analysis/o4_validation_20261028'))
    parser.add_argument('--binary', type=Path, default=Path('build/o4_detect'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [(p, 'air3s_present_weak_label') for p in sorted(Path('data/air3s').glob('*.iq'))]
    cases += [(p, 'non_o4_mini2_present') for p in sorted(Path('data/mini2').rglob('*.iq'))]
    cases += [(p, 'no_drone_baseline') for p in sorted(Path('data/baseline').glob('*.iq'))]
    wifi = Path('data/analysis/odid_20261028/baseline_ch6.iq')
    if wifi.exists():
        cases.append((wifi, 'no_drone_wifi_baseline'))
    if not cases:
        raise RuntimeError('No local recordings found')
    results = []
    for path, label in cases:
        result = evaluate(path, label, args.binary, args.output)
        results.append(result)
        print(f'{path.name}: {result["summary"]["candidate_windows"]}/{result["summary"]["windows"]} candidate windows', flush=True)
        (args.output/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    controls = [r for r in results if r['label'] != 'air3s_present_weak_label']
    findings = dict(profile='o4_structure_v2', recordings=len(results),
                    seconds=sum(r['duration_seconds'] for r in results),
                    control_seconds=sum(r['duration_seconds'] for r in controls),
                    control_candidate_windows=sum(r['summary']['candidate_windows'] for r in controls),
                    control_windows=sum(r['summary']['windows'] for r in controls),
                    o4_identity_accuracy=None,
                    limitation='Development used first 0.32 s of these sessions. Later windows are temporal extension, not independent held-out identity validation. Labels are recording-level; adjacent windows are correlated.')
    (args.output/'metrics.json').write_text(json.dumps(findings, indent=2)+'\n')
    lines = ['# O4 candidate detector local validation', '', findings['limitation'], '',
             '| Recording | Label | Seconds | Candidate / evaluated windows | Candidates after development prefix |',
             '|---|---|---:|---:|---:|']
    for r in results:
        lines.append(f'| {Path(r["path"]).name} | {r["label"]} | {r["duration_seconds"]:.3f} | {r["summary"]["candidate_windows"]} / {r["summary"]["windows"]} | {r["later_candidate_windows"]} |')
    lines += ['', f'Control candidate windows: {findings["control_candidate_windows"]}/{findings["control_windows"]} over {findings["control_seconds"]:.3f} s.',
              '', 'No independent O4 identity accuracy, unique drone count, range or direction claim. No large IQ downloads.']
    (args.output/'REPORT.md').write_text('\n'.join(lines)+'\n')


if __name__ == '__main__':
    main()
