"""Hardware-free regression for the actual V1.3 build/flash entrypoint."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

FW = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(FW / 'Rehab/scripts'))
import project


class ProjectEntryTests(unittest.TestCase):
    def setUp(self):
        (FW / 'build/rehab/tests').mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=FW / 'build/rehab/tests')
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.report = {'status': 'passed', 'board': 'v13', 'profile': 'peripherals',
                       'real_motor_enabled': True, 'native_control': True, 'motion_policy': 'native_board_control',
                       'source_sha256': 'source', 'artifact_sha256': {}}
        for name in project.FILENAMES:
            data = ('fixture ' + name).encode()
            (self.folder / name).write_bytes(data)
            self.report['artifact_sha256'][name] = hashlib.sha256(data).hexdigest()
        (self.folder / 'endpoints.hpp').write_text('rehab board_revision hardware_locked')
        self.write_report()

    def write_report(self):
        (self.folder / 'build.json').write_text(json.dumps(self.report))

    def validate(self, profile='peripherals'):
        with patch.object(project.firmware_build, 'source_digest', return_value='source'), \
                patch.object(project.resources, 'inspect', return_value={'errors': []}), \
                contextlib.redirect_stdout(io.StringIO()):
            return project.validate(self.folder, profile=profile)

    def test_verified_artifact(self):
        self.assertEqual(self.validate(), self.folder / 'ODriveFirmware.elf')

    def test_full_label_and_resource_selection(self):
        self.report['profile']='full';self.write_report()
        output=io.StringIO()
        with patch.object(project.firmware_build,'source_digest',return_value='source'), \
                patch.object(project.resources,'inspect',return_value={'errors':[]}) as inspect, \
                contextlib.redirect_stdout(output):
            project.validate(self.folder)
        inspect.assert_called_once_with('full','v13',folder=self.folder)
        self.assertIn('boot IDLE',output.getvalue())
        self.assertIn('training requires explicit Acquire/Enable/Start',output.getvalue())
        self.assertNotIn('LOCKED',output.getvalue())

    def test_wrong_metadata_fails_closed(self):
        for key, value in [('board', 'original'), ('board', 'v12'), ('status', 'failed'),
                           ('profile', 'native'), ('real_motor_enabled', False),
                           ('real_motor_enabled', 0), ('motion_policy', 'original_board_native'),
                           ('native_control', False), ('native_control', 1), ('native_control', None),
                           ('source_sha256', 'stale')]:
            with self.subTest(key=key, value=value):
                previous = self.report[key]
                self.report[key] = value
                self.write_report()
                with self.assertRaises(RuntimeError): self.validate()
                self.report[key] = previous
                self.write_report()

    def test_tampered_or_missing_output_fails_closed(self):
        for name in project.FILENAMES:
            with self.subTest(name=name):
                p = self.folder / name
                original = p.read_bytes()
                p.write_bytes(b'wrong board or corrupt image')
                with self.assertRaises(RuntimeError): self.validate()
                p.unlink()
                with self.assertRaises(RuntimeError): self.validate()
                p.write_bytes(original)

    def test_missing_v13_interface(self):
        (self.folder / 'endpoints.hpp').write_text('axis0 axis1')
        with self.assertRaises(RuntimeError): self.validate()

    def test_resource_failure(self):
        with patch.object(project.firmware_build, 'source_digest', return_value='source'), \
                patch.object(project.resources, 'inspect', return_value={'errors': ['wrong ELF symbols']}):
            with self.assertRaises(RuntimeError): project.validate(self.folder, profile='peripherals')

    def test_openocd_uses_dedicated_elf_and_readback(self):
        command = project.programmer_command(project.ARTIFACTS / 'ODriveFirmware.elf', 'ABC123')
        self.assertIn('flash write_image erase {build/rehab/v13/full/ODriveFirmware.elf}', command)
        self.assertIn('verify_image {build/rehab/v13/full/ODriveFirmware.elf}', command)
        self.assertLess(command.index('verify_image {build/rehab/v13/full/ODriveFirmware.elf}'),
                        command.index('reset run'))
        self.assertIn('hla_serial {ABC123}', command)
        for serial in ('bad;shutdown', 'bad} reset run', 'bad\nreset'):
            with self.assertRaises(RuntimeError): project.programmer_command(project.ARTIFACTS / 'ODriveFirmware.elf', serial)

    def exercise(self, action, dry_run=False, build_error=False, validate_error=False):
        events = []
        def build(*args, **kwargs):
            events.append(('build', args, kwargs))
            if build_error: raise RuntimeError('compile failed')
        def validate(*args, **kwargs):
            events.append('validate')
            if validate_error: raise RuntimeError('invalid output')
        with patch.object(project, 'project_lock', contextlib.nullcontext), \
                patch.object(project, 'run_guard', side_effect=lambda: events.append('guard')), \
                patch.object(project.firmware_build, 'build', side_effect=build), \
                patch.object(project, 'validate', side_effect=validate), \
                patch.object(project.subprocess, 'run', side_effect=lambda *a, **k: events.append('programmer')), \
                contextlib.redirect_stdout(io.StringIO()):
            if build_error or validate_error:
                with self.assertRaises(RuntimeError): project.execute(action, dry_run)
            else: project.execute(action, dry_run)
        return events

    def test_build_and_preflight_never_program(self):
        for action, dry_run in [('build', False), ('check', False), ('flash', True)]:
            events = self.exercise(action, dry_run)
            self.assertNotIn('programmer', events)
            self.assertIn('validate', events)

    def test_flash_order_and_fixed_target(self):
        self.assertEqual(self.exercise('flash'),
                         ['guard', ('build', ('full',), {'jobs': 1, 'board': 'v13'}),
                          'guard', 'validate', 'programmer'])

    def test_failed_build_or_validation_never_programs(self):
        self.assertNotIn('programmer', self.exercise('flash', build_error=True))
        self.assertNotIn('programmer', self.exercise('flash', validate_error=True))

    def test_lock_rejects_second_operation_and_releases(self):
        with patch.object(project, 'FW', self.folder):
            with project.project_lock():
                with self.assertRaises(RuntimeError):
                    with project.project_lock(): self.fail('concurrent operation admitted')
            with project.project_lock(): pass

    def test_full_metadata_and_cross_profile_rejection(self):
        self.report.update(profile='full', native_control=True, real_motor_enabled=True,
                           motion_policy='native_board_control')
        self.write_report()
        self.assertEqual(self.validate('full'), self.folder / 'ODriveFirmware.elf')
        with self.assertRaises(RuntimeError): self.validate('peripherals')
        for key in ('real_motor_enabled','native_control'):
            self.report[key]=False;self.write_report()
            with self.assertRaises(RuntimeError): self.validate('full')
            self.report[key]=True;self.write_report()

    def test_peripheral_fallback_target(self):
        with patch.object(project,'project_lock',contextlib.nullcontext), \
                patch.object(project,'run_guard'), \
                patch.object(project.firmware_build,'build') as build, \
                patch.object(project,'validate') as validate, \
                patch.object(project.subprocess,'run') as programmer:
            project.execute('build',profile='peripherals')
            build.assert_called_once_with('peripherals',jobs=1,board='v13')
            validate.assert_called_once_with(project.ARTIFACTS.parent/'peripherals',profile='peripherals')
            programmer.assert_not_called()

    def test_retired_profile_rejected_before_lock_or_program(self):
        for profile in ('bench','bad','training','statistics'):
            with patch.object(project, 'project_lock') as lock:
                with self.assertRaises(RuntimeError): project.execute('flash', profile=profile)
                lock.assert_not_called()
            with self.assertRaises(RuntimeError): self.validate(profile)

    def test_cli_and_vscode_default_target(self):
        with patch.object(project.sys, 'argv', ['project.py','check']), patch.object(project, 'execute') as execute:
            self.assertEqual(project.main(),0)
            execute.assert_called_once_with('check',False,'','full')
        tasks=json.loads((FW/'.vscode/tasks.json').read_text())['tasks']
        for label in ('build','flash - ST-Link'):
            task=next(t for t in tasks if t['label']==label)
            self.assertNotIn('--profile',task['args'])
        launch=(FW/'.vscode/launch.json').read_text()
        self.assertIn('/build/rehab/v13/full/ODriveFirmware.elf',launch)
        config=json.loads((FW/'.vscode/c_cpp_properties.json').read_text())['configurations'][0]
        self.assertIn('/v13/editor/',config['compileCommands'])
        self.assertIn('REHAB_STAGE=6',config['defines'])
        self.assertIn('REHAB_ENABLE_APPLICATION=1',config['defines'])


if __name__ == '__main__':
    unittest.main()
