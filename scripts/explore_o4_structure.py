#!/usr/bin/env python3
"""Development-only structure measurements; recording labels are not event truth."""
import json
from pathlib import Path
import numpy as np


def features(x, rate):
    x = x - x.mean()
    lag = round(rate / 30000)
    n = len(x) - lag - round(rate * 2e-6)
    a = x[:n]
    den = np.sqrt(np.sum(abs(a)**2) * np.sum(abs(x[lag:lag+n])**2))
    if den <= 1e-20:
        return [0.] * 5
    y = np.conj(a) * x[lag:lag+n]
    t = np.arange(n) / rate
    cycle = [abs(np.sum(y * np.exp(-2j*np.pi*f*t))) / den
             for f in [26000, 27000, 28000, 29000, 30000, 56000]]
    sides = [abs(np.vdot(a, x[l:l+n])) / den
             for l in [lag-round(rate*2e-6), lag-round(rate*1e-6),
                       lag+round(rate*1e-6), lag+round(rate*2e-6)]]
    cp = abs(y.sum()) / den
    return [float(cp), float(cp / max(np.median(sides), 1e-6)),
            float(cycle[2]), float(cycle[2]/max(np.median(cycle[:2]+cycle[3:5]), 1e-6)),
            float(cycle[5])]


if __name__ == '__main__':
    output = []
    paths = sorted(Path('data/air3s').glob('*.iq')) + sorted(Path('data/baseline').glob('*.iq'))
    paths += sorted(Path('data/mini2').rglob('*.iq'))
    paths += [Path('data/analysis/odid_20261028/baseline_ch6.iq')]
    for p in paths:
        meta = dict(line.replace(' ', '').split('=', 1) for line in Path(str(p)+'.meta').read_text().splitlines() if '=' in line)
        rate = float(meta['sample_rate'])
        raw = np.fromfile(p, np.int16, count=2*int(rate*.32)).reshape(-1,2)
        x = (raw[:,0]+1j*raw[:,1])/32768
        size = int(rate*.016)
        rows = [features(x[i:i+size], rate) for i in range(0,len(x)-size+1,size)]
        hits = sum(a>.03 and b>3 and c>.02 and d>1.5 and e>.01 for a,b,c,d,e in rows)
        print(p.name, hits, len(rows), np.round(np.median(rows,axis=0),3), flush=True)
        output.append(dict(path=str(p), features=rows, development_hits=hits))
    Path('data/analysis/o4_development_20261028').mkdir(exist_ok=True)
    Path('data/analysis/o4_development_20261028/structure.json').write_text(json.dumps(output,indent=2))
