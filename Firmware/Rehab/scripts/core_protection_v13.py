"""Two immutable history layers plus an explicitly reviewed migration surface.

The 2026-10-29 snapshot is NOT a replacement for the original baseline. Its
inherited differences must be accounted for against the original manifest.
No algorithm file or V1.2 board file is newly exempted by this migration.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import zipfile

FW=pathlib.Path(__file__).resolve().parents[2]
REPO=FW.parent
ORIGINAL=REPO/'analysis/rehab_v12_baseline'
SNAPSHOT=REPO/'analysis/rehab_v13_baseline'
REVIEWED=FW/'Rehab/core_manifest_v13.json'
# These are the only pre-existing integration hooks changed by V1.3.
# The reviewed manifest records their exact bytes after independent review.
HOOKS={ 'Firmware/'+p for p in (
    'Drivers/STM32/stm32_gpio.cpp','Drivers/STM32/stm32_spi_arbiter.hpp',
    'MotorControl/nvm_config.hpp','Tupfile.lua') }
EXTENSIONS=('Firmware/Rehab/','Firmware/Tests/rehab/','Firmware/Board/rehab_v13/')
NEW=EXTENSIONS+('analysis/rehab_v13_baseline/',)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--freeze',action='store_true',help='Record exact integration bytes ONLY after independent review.')
    p.add_argument('--audit-only',action='store_true',help='Verify history and boundaries, without claiming integration review.')
    a=p.parse_args()
    errors=[]
    old=json.loads((ORIGINAL/'manifest.json').read_text(encoding='utf-8'))
    current=json.loads((SNAPSHOT/'manifest.json').read_text(encoding='utf-8'))
    for base in (ORIGINAL,SNAPSHOT):
        for name,digest in json.loads((base/'SHA256.json').read_text(encoding='utf-8')).items():
            if sha(base/name)!=digest: errors.append('Immutable archive changed: '+str(base/name))
    expected=[{'path':name,'historical_sha256':digest,'inherited_sha256':current['files'].get(name)}
              for name,digest in old['files'].items() if digest!=current['files'].get(name)]
    if sorted(expected,key=lambda x:x['path'])!=sorted(current['inherited_changes'],key=lambda x:x['path']):
        errors.append('Inherited history delta is missing, extra, or inconsistent')
    # Verify the full captured source archive, not merely its JSON hash list.
    with zipfile.ZipFile(SNAPSHOT/'worktree.zip') as z:
        for name,digest in current['files'].items():
            try: actual=hashlib.sha256(z.read(name)).hexdigest()
            except KeyError: actual=None
            if actual!=digest: errors.append('Snapshot content mismatch: '+name)
    for name,digest in current['files'].items():
        if name not in HOOKS and not name.startswith(EXTENSIONS) and sha(REPO/name)!=digest:
            errors.append('Changed outside V1.3 integration surface: '+name)
    # The original reviewed manifest is historical evidence, not a new hash target.
    old_review='Firmware/Rehab/core_manifest.json'
    if sha(REPO/old_review)!=current['files'].get(old_review): errors.append('Original reviewed manifest changed')
    listed=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=REPO,text=True,encoding='utf-8')
    for name in set(filter(None,listed.split('\0'))):
        if name not in current['files'] and not name.startswith(NEW): errors.append('New file outside V1.3 extension surface: '+name)
    if errors:
        print('\n'.join(errors),file=sys.stderr);return 1
    if a.audit_only:
        print(f"AUDIT: both history archives verified; {len(current['inherited_changes'])} inherited differences retained; integration review still required.")
        return 0
    if a.freeze:
        paths=set(HOOKS)
        paths.update(f.relative_to(REPO).as_posix() for f in (FW/'Board/rehab_v13').rglob('*') if f.is_file())
        # Review-critical extension sources and the checker itself are frozen too.
        paths.update(f.relative_to(REPO).as_posix() for f in (FW/'Rehab').rglob('*')
            if f.is_file() and f.suffix in ('.cpp','.hpp','.h','.py') and f.name!='core_manifest_v13.json')
        data={'original_manifest_sha256':sha(ORIGINAL/'manifest.json'),
              'snapshot_manifest_sha256':sha(SNAPSHOT/'manifest.json'),
              'snapshot_head':current['head'],
              'review_record':'Firmware/Rehab/docs/v13_review.md',
              'reviewed_files':{name:sha(REPO/name) for name in sorted(paths)}}
        if not (FW/'Rehab/docs/v13_review.md').is_file():
            print('Independent review record is required before freeze.',file=sys.stderr);return 1
        REVIEWED.write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
        print('Recorded exact V1.3 integration hashes; original evidence unchanged.');return 0
    if not REVIEWED.is_file():
        print('V1.3 integration is not yet registered after review.',file=sys.stderr);return 1
    reviewed=json.loads(REVIEWED.read_text(encoding='utf-8'))
    if reviewed['original_manifest_sha256']!=sha(ORIGINAL/'manifest.json') or reviewed['snapshot_manifest_sha256']!=sha(SNAPSHOT/'manifest.json'):
        errors.append('Historical manifest identity changed')
    for name,digest in reviewed['reviewed_files'].items():
        if sha(REPO/name)!=digest: errors.append('Reviewed V1.3 file changed: '+name)
    if errors:
        print('\n'.join(errors),file=sys.stderr);return 1
    print(f"PASS: original history + {len(current['files'])}-file inherited snapshot; {len(reviewed['reviewed_files'])} reviewed V1.3 files; original algorithms and V1.2 board preserved.")
    return 0

if __name__=='__main__': sys.exit(main())
