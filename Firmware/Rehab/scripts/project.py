#!/usr/bin/env python3
"""V1.3 peripherals/native-control entrypoint. Only the flash action accesses hardware."""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import build as firmware_build
import resources

FW = Path(__file__).resolve().parents[2]
ARTIFACTS = FW / 'build/rehab/v13/peripherals'
FILENAMES = tuple('ODriveFirmware.' + suffix for suffix in ('elf', 'hex', 'bin', 'map'))


def run_guard():
    subprocess.run([sys.executable, '-B', str(FW / 'Rehab/scripts/check_core.py')],
                   cwd=FW, check=True)


def validate(folder=ARTIFACTS, profile='peripherals'):
    """Reject failed, mismatched, stale or modified output before programmer launch."""
    if profile not in ('peripherals', 'full'): raise RuntimeError('Unsupported project profile')
    report = json.loads((folder / 'build.json').read_text(encoding='utf-8'))
    native = profile == 'peripherals'
    required = {'status': 'passed', 'board': 'v13', 'profile': profile,
                'real_motor_enabled': native, 'native_control': native,
                'motion_policy': 'native_board_control' if native else 'rehab_inhibited'}
    for key, value in required.items():
        if report.get(key) != value or (isinstance(value, bool) and report.get(key) is not value):
            raise RuntimeError('Refusing artifact: invalid ' + key)
    if report.get('source_sha256') != firmware_build.source_digest():
        raise RuntimeError('Refusing artifact: sources changed since build')
    for name in FILENAMES:
        path = folder / name
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError('Refusing artifact: missing/empty ' + name)
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != report.get('artifact_sha256', {}).get(name):
            raise RuntimeError('Refusing artifact: hash mismatch for ' + name)
    result = resources.inspect(profile, 'v13', folder=folder)
    if result['errors']:
        raise RuntimeError('Refusing artifact: ' + '; '.join(result['errors']))
    endpoints = (folder / 'endpoints.hpp').read_text(encoding='utf-8')
    if not all(name in endpoints for name in ('rehab', 'board_revision', 'hardware_locked')):
        raise RuntimeError('Refusing artifact: missing V1.3 interface')
    elf = folder / 'ODriveFirmware.elf'
    print('VERIFIED: rehab-v1.3 / ' + profile + (' / native motor commands CAPABLE; boot IDLE, explicit command required'
          if native else ' / real motor LOCKED'), flush=True)
    print('ELF: ' + str(elf), flush=True)
    print('SHA256: ' + report['artifact_sha256'][elf.name], flush=True)
    return elf


def programmer_command(elf, serial=''):
    if serial and not re.fullmatch(r'[A-Za-z0-9_.:-]+', serial):
        raise RuntimeError('Invalid ST-Link serial: only letters, digits, _, ., :, - are allowed')
    # Fixed project-relative path avoids Windows escaping/Tcl command injection.
    path = elf.relative_to(FW).as_posix()
    if any(char in path for char in '{}\n\r'):
        raise RuntimeError('Unsupported programmer path')
    command = ['openocd', '-f', 'interface/stlink-v2.cfg', '-f', 'target/stm32f4x.cfg']
    if serial:
        command += ['-c', 'hla_serial {' + serial + '}']
    command += ['-c', 'init', '-c', 'reset halt',
                '-c', 'flash write_image erase {' + path + '}',
                '-c', 'verify_image {' + path + '}',
                '-c', 'reset run', '-c', 'shutdown']
    return command


@contextlib.contextmanager
def project_lock():
    path = FW / 'build/rehab/project.lock'
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        handle = path.open('x', encoding='utf-8')
    except FileExistsError:
        raise RuntimeError('Another project operation is active (build/rehab/project.lock). '
                           'If interrupted, confirm it has exited before removing that lock.') from None
    try:
        with handle:
            handle.write(str(os.getpid()))
        yield
    finally:
        path.unlink()


def execute(action, dry_run=False, serial='', profile='peripherals'):
    # Reject malformed parameters before starting a build, with no device access.
    if action not in ('build', 'check', 'flash') or profile not in ('peripherals', 'full'):
        raise RuntimeError('Unsupported action/profile')
    folder = ARTIFACTS if profile == 'peripherals' else FW/'build/rehab/v13/full'
    command = programmer_command(folder / 'ODriveFirmware.elf', serial)
    with project_lock():
        run_guard()
        if action in ('build', 'flash'):
            firmware_build.build(profile, jobs=1, board='v13')
        run_guard()
        validate(folder, profile=profile)
        if action == 'flash':
            print('PROGRAMMER argv: ' + json.dumps(command), flush=True)
            if dry_run:
                print('DRY RUN: programmer NOT executed; no hardware accessed.', flush=True)
            else:
                subprocess.run(command, cwd=FW, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('build', 'check', 'flash'))
    parser.add_argument('--dry-run', action='store_true', help='flash preflight only; never launch OpenOCD')
    parser.add_argument('--serial', default='', help='Optional ST-Link serial number')
    parser.add_argument('--profile', choices=('peripherals', 'full'), default='peripherals')
    args = parser.parse_args()
    try:
        execute(args.action, args.dry_run, args.serial, args.profile)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print('FAILED (no further commands executed): ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
