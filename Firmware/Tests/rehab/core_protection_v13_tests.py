"""Exercise the production guard without changing files or the Git index."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
from unittest.mock import patch

FW=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('v13_guard',FW/'Rehab/scripts/core_protection_v13.py')
guard=importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
real_sha=guard.sha
real_git=guard.subprocess.check_output

def run(**patches):
    with contextlib.ExitStack() as stack:
        stack.enter_context(patch.object(sys,'argv',['core_protection_v13.py','--audit-only']))
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
        for name,value in patches.items(): stack.enter_context(patch.object(guard,name,value))
        return guard.main()

assert run()==0
# A new path must be rejected even if Git returns it as cached (staged), not
# untracked. The index is never edited by this test.
def staged_source(command,**kwargs):
    assert '--cached' in command and '--others' in command
    return real_git(command,**kwargs)+'Firmware/MotorControl/unreviewed_new_algorithm.cpp\0'
with patch.object(guard.subprocess,'check_output',staged_source): assert run()==1

for target in ('Firmware/MotorControl/controller.cpp','Firmware/MotorControl/encoder.cpp',
               'Firmware/Board/rehab_v12/board.cpp','Firmware/Drivers/DRV8301/drv8301.cpp',
               'Firmware/Rehab/core_manifest.json','analysis/rehab_v13_baseline/worktree.zip'):
    def altered(path,target=target):
        return '0'*64 if Path(path)==guard.REPO/target else real_sha(path)
    assert run(sha=altered)==1,target
assert run()==0
print('PASS: production guard rejects staged core additions, core/V1.2/DRV changes and historical evidence tampering; no files or index changed')
