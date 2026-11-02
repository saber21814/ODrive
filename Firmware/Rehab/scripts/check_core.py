#!/usr/bin/env python3
"""Verify upstream/local baseline protection and the reviewed integration surface."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

FW = pathlib.Path(__file__).resolve().parents[2]
REPO = FW.parent
BASE = REPO / 'analysis/rehab_v12_baseline'
MANIFEST = FW / 'Rehab/core_manifest.json'
HOOKS = [
    'Drivers/STM32/stm32_gpio.cpp',
    'Drivers/STM32/stm32_spi_arbiter.hpp',
    'MotorControl/axis.cpp', 'MotorControl/low_level.cpp', 'MotorControl/main.cpp',
    'MotorControl/motor.cpp', 'MotorControl/nvm_config.hpp', 'MotorControl/odrive_main.h',
    'MotorControl/thermistor.cpp', 'Tupfile.lua',
    'communication/ascii_protocol.cpp', 'communication/can/can_simple.cpp',
    'communication/interface_i2c.cpp', 'communication/interface_uart.cpp',
    'communication/interface_usb.cpp', 'fibre-cpp/endpoints_template.j2',
    'fibre-cpp/function_stubs_template.j2', 'fibre-cpp/protocol.hpp',
]
ALLOW = {'Firmware/' + path for path in HOOKS}
# Reviewed editor integration is separate from the native algorithm hooks.
ALLOW.update('Firmware/.vscode/'+name for name in
             ('c_cpp_properties.json','settings.json','tasks.json'))
NEW_PREFIXES = ('Firmware/Rehab/', 'Firmware/Tests/rehab/', 'Firmware/Board/rehab_v12/',
                'docs/rehab_v12/', 'analysis/rehab_v12_baseline/')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None


def main():
    if '--legacy' not in sys.argv and (REPO/'analysis/rehab_v13_baseline/manifest.json').is_file():
        from core_protection_v13 import main as v13_main
        return v13_main()
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--legacy',action='store_true',help='Run the historical pre-merge check without inherited-delta accounting.')
    p.add_argument('--freeze', action='store_true', help='Record reviewed hook/board hashes AFTER review; never run automatically.')
    a = p.parse_args()
    baseline = json.loads((BASE / 'manifest.json').read_text(encoding='utf-8'))
    errors = []
    archives = json.loads((BASE / 'SHA256.json').read_text(encoding='utf-8'))
    for name, sha in archives.items():
        if digest(BASE / name) != sha: errors.append('Baseline archive changed: ' + name)
    for name, sha in baseline['files'].items():
        if name not in ALLOW and digest(REPO / name) != sha:
            errors.append('Outside integration whitelist: ' + name)
    untracked = subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], cwd=REPO, text=True)
    for name in untracked.splitlines():
        if name not in baseline['files'] and not name.startswith(NEW_PREFIXES):
            errors.append('New file outside extension whitelist: ' + name)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    if a.freeze:
        reviewed = {name: digest(REPO / name) for name in sorted(ALLOW)}
        for f in sorted((FW/'Board/rehab_v12').rglob('*')):
            if f.is_file(): reviewed[f.relative_to(REPO).as_posix()] = digest(f)
        MANIFEST.write_text(json.dumps({'baseline_head': baseline['head'],
            'baseline_manifest_sha256': digest(BASE/'manifest.json'), 'reviewed_files': reviewed}, indent=2)+'\n', encoding='utf-8')
        print('Recorded reviewed integration hashes; protected baseline unchanged.')
        return 0
    if not MANIFEST.exists():
        print('Missing reviewed core_manifest.json; review changes before --freeze.', file=sys.stderr)
        return 1
    approved = json.loads(MANIFEST.read_text(encoding='utf-8'))
    if approved['baseline_manifest_sha256'] != digest(BASE/'manifest.json'):
        errors.append('Baseline manifest identity changed')
    for name, sha in approved['reviewed_files'].items():
        if digest(REPO / name) != sha: errors.append('Reviewed integration file changed: ' + name)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print(f"PASS: {len(baseline['files'])} baseline paths, {len(approved['reviewed_files'])} reviewed board/hooks; pre-existing changes preserved.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
