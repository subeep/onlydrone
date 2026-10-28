#!/usr/bin/env python3
"""Exercise built Windows binaries on Windows or via a supplied Wine executable."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build/windows'))
    parser.add_argument('--runner', help='Optional Wine executable; environment configures Wine')
    parser.add_argument('--output', type=Path, default=Path('data/analysis/windows_20261028'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    prefix = [args.runner] if args.runner else []
    results = []

    def run(name, binary, options=(), expected=0, contains=()):
        command = prefix + [str(args.build/binary)] + list(options)
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
        (args.output/(name+'.log')).write_text(result.stdout)
        passed = result.returncode == expected and all(s in result.stdout for s in contains)
        results.append(dict(name=name, command=command, exit_code=result.returncode, passed=passed))
        (args.output/'checks.json').write_text(json.dumps(results, indent=2)+'\n')
        print(name, 'PASS' if passed else 'FAIL', flush=True)

    for name in ['test_ring_buffer', 'test_file_source', 'test_config', 'test_receiver',
                 'test_ui_model', 'test_odid', 'test_odid_wifi', 'test_ui_worker',
                 'test_dji', 'test_resampler', 'test_rf_metrics', 'test_o4_detector']:
        run(name, 'tests/'+name+'.exe', contains=['0 failing'])
    run('replay-mini2', 'droneid_ui.exe', ['--check','--replay','tests/data/mini2_wideband.cf32',
        '--format','cf32','--rate','50e6','--auto-center'], contains=['packets=1 tracks=1','serial=SysSecWasHere','inner_crc16=OK'])
    run('missing-input', 'droneid_ui.exe', ['--check','--replay','does-not-exist.iq'], expected=1, contains=['status=Error'])
    # No hardware needed. Invalid sample rate must fail before receiver startup.
    run('invalid-rate', 'droneid_ui.exe', ['--check','--replay','tests/data/mini2_wideband.cf32','--rate','nan'], expected=2)
    if not all(r['passed'] for r in results):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
