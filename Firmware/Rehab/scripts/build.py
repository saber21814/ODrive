#!/usr/bin/env python3
"""Build only firmware; preserve the user's tup.config and host generated files."""
import argparse, base64, hashlib, json, pathlib, shutil, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'build' / 'rehab'
PROFILES = {
    'native': (1, ''),
    'hx711': (2, 'HX711'),
    'network': (2, 'NETWORK PROTOCOL'),
    'peripherals': (2, 'HX711 NETWORK PROTOCOL'),
    'protection': (3, 'HX711 NETWORK PROTOCOL APPLICATION'),
    'training': (4, 'HX711 NETWORK PROTOCOL APPLICATION TRAINING'),
    'statistics': (5, 'HX711 NETWORK PROTOCOL APPLICATION TRAINING STATISTICS'),
    'full': (6, 'HX711 NETWORK PROTOCOL APPLICATION TRAINING STATISTICS COMPENSATION'),
}
OPTIONS = 'HX711 NETWORK PROTOCOL APPLICATION TRAINING STATISTICS COMPENSATION'.split()
RECOVERY = OUT / 'tup-config-recovery.json'

def recover_config():
    if not RECOVERY.exists(): return
    saved=json.loads(RECOVERY.read_text(encoding='utf-8'))
    config=ROOT/'tup.config'
    current=config.read_bytes() if config.exists() else None
    current_sha=hashlib.sha256(current).hexdigest() if current is not None else None
    if current_sha not in (saved['original_sha256'],saved['temporary_sha256']):
        raise RuntimeError('tup.config changed after interruption; recovery journal retained for manual comparison.')
    original=base64.b64decode(saved['original_base64']) if saved['existed'] else None
    if original is None: config.unlink(missing_ok=True)
    else: config.write_bytes(original)
    RECOVERY.unlink()
    print('Restored tup.config from the interrupted build journal.')

def source_digest():
    digest = hashlib.sha256()
    paths = []
    for directory in ('Board', 'Rehab', 'MotorControl', 'Drivers', 'communication', 'fibre-cpp'):
        paths += [p for p in (ROOT/directory).rglob('*') if p.is_file() and p.suffix in ('.c','.cpp','.h','.hpp','.j2','.lua','.ld','.s','.yaml')]
    paths += [ROOT/'Tupfile.lua', ROOT/'odrive-interface.yaml', ROOT/'Rehab/interface.py']
    for path in sorted(set(paths)):
        digest.update(path.relative_to(ROOT).as_posix().encode());digest.update(path.read_bytes())
    return digest.hexdigest()

def build(profile, jobs=1, board='v12'):
    OUT.mkdir(parents=True, exist_ok=True)
    dest = (OUT / 'v13' if board == 'v13' and profile != 'baseline' else OUT) / profile
    dest.mkdir(parents=True, exist_ok=True)
    config = ROOT / 'tup.config'
    previous = config.read_bytes() if config.exists() else None
    if RECOVERY.exists(): raise RuntimeError('Interrupted/concurrent build journal exists; use --recover-config before building.')
    config_text = (previous or b'').decode('utf-8-sig')
    keep = [l for l in config_text.splitlines() if not l.startswith(('CONFIG_BOARD_VERSION=', 'CONFIG_REHAB_', 'CONFIG_DOCTEST=', 'CONFIG_DEBUG='))]
    if profile == 'baseline':
        keep += ['CONFIG_BOARD_VERSION=v3.6-56V', 'CONFIG_DOCTEST=false', 'CONFIG_DEBUG=false']
    else:
        stage, enabled = PROFILES[profile]
        keep += ['CONFIG_BOARD_VERSION='+('rehab-v1.3' if board == 'v13' else 'rehab-v1.2'), 'CONFIG_DOCTEST=false', 'CONFIG_DEBUG=false', f'CONFIG_REHAB_STAGE={stage}', 'CONFIG_REHAB_REAL_MOTOR=false']
        keep += [f'CONFIG_REHAB_{o}={str(o in enabled.split()).lower()}' for o in OPTIONS]
    print(f'Building {profile}; outputs {dest}', flush=True)
    started = time.time()
    source_sha = source_digest()
    (dest/'build.json').write_text(json.dumps({'profile':profile,'status':'building','source_sha256':source_sha}), encoding='utf-8')
    try:
        temporary=('\n'.join(keep)+'\n').encode('utf-8')
        RECOVERY.write_text(json.dumps({'existed':previous is not None,
            'original_base64':base64.b64encode(previous or b'').decode('ascii'),
            'original_sha256':hashlib.sha256(previous).hexdigest() if previous is not None else None,
            'temporary_sha256':hashlib.sha256(temporary).hexdigest()},indent=2),encoding='utf-8')
        config.write_bytes(temporary)
        subprocess.run([sys.executable, '-B', str(ROOT.parent/'tools/odrive/version.py'), '--output', 'autogen/version.c'], cwd=ROOT, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with (dest/'build.log').open('w', encoding='utf-8') as log:
            # Serial jobs avoid Windows tup temporary-output races and allow the
            # same command to run reliably alongside host test compilation.
            result = subprocess.run(['tup', '--quiet', '-no-environ-check', f'-j{jobs}'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        # Retry only the known Windows tup temporary-output race, serially.
        # Compilation/link failures are not retried or hidden.
        first_log=(dest/'build.log').read_text(encoding='utf-8',errors='replace')
        if result.returncode and jobs>1 and '.tup/tmp/' in first_log and ('Unable to open file for comparison' in first_log or 'Unable to unlink sub-process output' in first_log):
            shutil.copy2(dest/'build.log', dest/'parallel-attempt.log')
            with (dest/'build.log').open('w',encoding='utf-8') as log:
                result=subprocess.run(['tup','--quiet','-no-environ-check','-j1'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        if result.returncode:
            print((dest/'build.log').read_text(encoding='utf-8', errors='replace')[-14000:])
            raise RuntimeError(f'{profile} failed; see {dest / "build.log"}')
        for suffix in ('elf','hex','bin','map'):
            shutil.copy2(ROOT/'build'/f'ODriveFirmware.{suffix}', dest/f'ODriveFirmware.{suffix}')
        shutil.copy2(ROOT/'autogen/endpoints.hpp', dest/'endpoints.hpp')
        if profile=='full':
            from editor import capture
            capture(board)
        size = subprocess.check_output(['arm-none-eabi-size', str(dest/'ODriveFirmware.elf')], text=True)
        if source_digest()!=source_sha: raise RuntimeError('Firmware sources changed during build; rebuild after edits finish.')
        report = {'profile':profile, 'board':'original' if profile=='baseline' else board, 'status':'passed', 'source_sha256':source_sha, 'elapsed_s':time.time()-started, 'compiler':subprocess.check_output(['arm-none-eabi-gcc','--version'], text=True).splitlines()[0], 'size':size,
                  'real_motor_enabled':None if profile=='baseline' else False,
                  'motion_policy':'original_board_native' if profile=='baseline' else 'rehab_inhibited',
                  'artifact_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in dest.glob('ODriveFirmware.*')}}
        (dest/'build.json').write_text(json.dumps(report,indent=2), encoding='utf-8')
        print(size, end='')
    except Exception:
        (dest/'build.json').write_text(json.dumps({'profile':profile,'status':'failed','source_sha256':source_sha}), encoding='utf-8')
        raise
    finally:
        if previous is None:
            config.unlink(missing_ok=True)
        else:
            config.write_bytes(previous)
        RECOVERY.unlink(missing_ok=True)

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('profile', choices=['baseline',*PROFILES,'all'])
    p.add_argument('--jobs',type=int,choices=range(1,9),default=1,help='Compiler jobs; default 1 for Windows tup reliability.')
    p.add_argument('--board',choices=['v12','v13'],default='v12',help='Separate Rehab board target and artifact directory.')
    p.add_argument('--recover-config',action='store_true',help='Restore the saved config after a killed build before continuing.')
    a=p.parse_args()
    try:
        if a.recover_config: recover_config()
        for name in ['baseline',*PROFILES] if a.profile=='all' else [a.profile]: build(name,a.jobs,a.board)
    except (RuntimeError,subprocess.CalledProcessError) as e:
        print(e,file=sys.stderr);sys.exit(1)
