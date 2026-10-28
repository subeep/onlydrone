#!/usr/bin/env python3
"""Copy UHD's C declarations, removing C++ implementation sections with unifdef.

Usage: python3 scripts/vendor_uhd_c.py /usr/include /path/to/unifdef
Generated declarations retain upstream copyright and their original ABI.
"""
import pathlib
import re
import subprocess
import sys

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(__file__).resolve().parents[1] / "third_party/uhd_c"
pending = ["uhd/config.h", "uhd/usrp/usrp.h", "uhd/version.h"]
seen = set()
while pending:
    name = pending.pop()
    if name in seen:
        continue
    seen.add(name)
    result = subprocess.run([sys.argv[2], "-U__cplusplus", str(source / name)], capture_output=True)
    if result.returncode not in (0, 1):
        raise RuntimeError(result.stderr.decode())
    content = result.stdout.decode()
    target = destination / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content)
    pending.extend(re.findall(r'#include\s+[<"](uhd/[^>"]+)[>"]', content))
print(f"Copied {len(seen)} C headers")
