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
        self.report = {'status': 'passed', 'board': 'v13', 'profile': 'full',
                       'real_motor_enabled': False, 'bench_mode': False, 'motion_policy': 'rehab_inhibited',
                       'source_sha256': 'source', 'artifact_sha256': {}}
        for name in project.FILENAMES:
            data = ('fixture ' + name).encode()
            (self.folder / name).write_bytes(data)
            self.report['artifact_sha256'][name] = hashlib.sha256(data).hexdigest()
        (self.folder / 'endpoints.hpp').write_text('rehab board_revision hardware_locked')
        self.write_report()

    def write_report(self):
        (self.folder / 'build.json').write_text(json.dumps(self.report))

    def validate(self, profile='full'):
        with patch.object(project.firmware_build, 'source_digest', return_value='source'), \
                patch.object(project.resources, 'inspect', return_value={'errors': []}), \
                contextlib.redirect_stdout(io.StringIO()):
            return project.validate(self.folder, profile=profile)

    def test_verified_artifact(self):
        self.assertEqual(self.validate(), self.folder / 'ODriveFirmware.elf')

    def test_wrong_metadata_fails_closed(self):
        for key, value in [('board', 'original'), ('board', 'v12'), ('status', 'failed'),
                           ('profile', 'native'), ('real_motor_enabled', True),
                           ('real_motor_enabled', 0), ('motion_policy', 'original_board_native'),
                           ('bench_mode', True), ('bench_mode', 0), ('bench_mode', None),
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
            with self.assertRaises(RuntimeError): project.validate(self.folder)

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
        def validate():
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

    def test_bench_metadata_and_cross_profile_rejection(self):
        self.report.update(profile='bench', bench_mode=True, real_motor_enabled=True,
                           motion_policy='bounded_native_bench')
        self.write_report()
        self.assertEqual(self.validate('bench'), self.folder/'ODriveFirmware.elf')
        with self.assertRaises(RuntimeError): self.validate('full')
        for key, value in [('bench_mode', 1), ('bench_mode', False), ('bench_mode', None),
                           ('real_motor_enabled', 1), ('real_motor_enabled', False),
                           ('motion_policy', 'rehab_inhibited'), ('board', 'v12'), ('profile', 'full')]:
            with self.subTest(key=key,value=value):
                previous=self.report[key];self.report[key]=value;self.write_report()
                with self.assertRaises(RuntimeError): self.validate('bench')
                self.report[key]=previous;self.write_report()

    def test_full_cannot_be_selected_as_bench(self):
        with self.assertRaises(RuntimeError): self.validate('bench')

    def test_bench_explicit_target_and_dry_run(self):
        for dry_run in (True, False):
            with patch.object(project, 'project_lock', contextlib.nullcontext), \
                    patch.object(project, 'run_guard'), \
                    patch.object(project.firmware_build, 'build') as build, \
                    patch.object(project, 'validate') as validate, \
                    patch.object(project.subprocess, 'run') as programmer, \
                    contextlib.redirect_stdout(io.StringIO()):
                project.execute('flash', dry_run=dry_run, profile='bench')
                build.assert_called_once_with('bench', jobs=1, board='v13')
                validate.assert_called_once_with(project.FW/'build/rehab/v13/bench', profile='bench')
                if dry_run: programmer.assert_not_called()
                else:
                    args=programmer.call_args.args[0]
                    self.assertIn('verify_image {build/rehab/v13/bench/ODriveFirmware.elf}',args)
        with patch.object(project, 'project_lock') as lock:
            with self.assertRaises(RuntimeError): project.execute('flash',profile='bad')
            lock.assert_not_called()

    def test_bench_validation_failure_never_programs(self):
        with patch.object(project, 'project_lock', contextlib.nullcontext), \
                patch.object(project, 'run_guard'), \
                patch.object(project.firmware_build, 'build'), \
                patch.object(project, 'validate', side_effect=RuntimeError('invalid bench')), \
                patch.object(project.subprocess, 'run') as programmer:
            with self.assertRaises(RuntimeError): project.execute('flash', profile='bench')
            programmer.assert_not_called()


if __name__ == '__main__':
    unittest.main()
