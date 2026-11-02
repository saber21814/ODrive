#!/usr/bin/env python3
"""One command: protected-core check, host tests, all ARM profiles, ELF resource checks."""
import argparse
import pathlib
import subprocess
import sys

HERE=pathlib.Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--host-only', action='store_true', help='Skip ARM builds and ELF inspection.')
p.add_argument('--board',choices=['v12','v13'],default='v12')
a=p.parse_args()
commands=[('check_core.py', []), ('test.py', [])]
if not a.host_only: commands += [('build.py', ['all','--board',a.board]), ('resources.py', ['all','--board',a.board])]
commands += [('check_core.py', [])]
for name, args in commands:
    result=subprocess.run([sys.executable, '-B', str(HERE/name), *args])
    if result.returncode: sys.exit(result.returncode)
print('PASS: requested verification complete; no hardware was accessed.')
