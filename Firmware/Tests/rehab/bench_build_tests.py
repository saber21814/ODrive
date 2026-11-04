"""Build policy tests with mocked tools; never compile ARM, program or access devices."""
import contextlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

FW=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(FW/'Rehab/scripts'))
import build
import editor
import resources

class BenchBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp_root=FW/'build/rehab/tests'
        self.temp_root.mkdir(parents=True,exist_ok=True)

    def test_v12_bench_rejected_before_writing(self):
        with patch.object(build, 'OUT') as out:
            with self.assertRaises(RuntimeError): build.build('bench',board='v12')
            out.mkdir.assert_not_called()

    def test_profile_isolation_report_and_config_restore(self):
        for profile,board in [('bench','v13'),('full','v13'),('full','v12'),('baseline','v12')]:
            with self.subTest(profile=profile,board=board), tempfile.TemporaryDirectory(dir=self.temp_root) as tmp:
                root=pathlib.Path(tmp);out=root/'build/rehab';out.mkdir(parents=True)
                (root/'autogen').mkdir()
                original=b'CONFIG_REHAB_BENCH=true\nCONFIG_REHAB_REAL_MOTOR=true\nCONFIG_BOARD_VERSION=old\n'
                (root/'tup.config').write_bytes(original)
                captured=[]
                def run(command,**kwargs):
                    if command[0]=='tup':
                        captured.append((root/'tup.config').read_text())
                        for suffix in ('elf','hex','bin','map'):
                            (root/'build'/('ODriveFirmware.'+suffix)).write_bytes(b'fixture')
                        (root/'autogen/endpoints.hpp').write_text('fixture')
                    return subprocess.CompletedProcess(command,0)
                with patch.multiple(build,ROOT=root,OUT=out,RECOVERY=out/'recovery.json'), \
                        patch.object(build,'source_digest',return_value='source'), \
                        patch.object(build.subprocess,'run',side_effect=run), \
                        patch.object(build.subprocess,'check_output',return_value='fixture'), \
                        patch.object(editor,'capture') as capture, contextlib.redirect_stdout(io.StringIO()):
                    build.build(profile,board=board)
                self.assertEqual((root/'tup.config').read_bytes(),original)
                self.assertFalse((out/'recovery.json').exists())
                self.assertEqual(len(captured),1)
                config=captured[0]
                bench=profile=='bench'
                self.assertIn('CONFIG_REHAB_BENCH='+str(bench).lower(),config)
                if profile!='baseline': self.assertIn('CONFIG_REHAB_REAL_MOTOR=false',config)
                if bench:
                    for option in build.OPTIONS: self.assertIn('CONFIG_REHAB_'+option+'=true',config)
                    self.assertIn('CONFIG_REHAB_STAGE=6',config)
                if profile=='full': capture.assert_called_once_with(board)
                else: capture.assert_not_called()
                folder=(out/'v13' if board=='v13' and profile!='baseline' else out)/profile
                report=json.loads((folder/'build.json').read_text())
                self.assertIs(report['bench_mode'],bench)
                self.assertIs(report['real_motor_enabled'],None if profile=='baseline' else bench)
                self.assertEqual(report['motion_policy'], 'original_board_native' if profile=='baseline' else
                                 'bounded_native_bench' if bench else 'rehab_inhibited')

    def test_resource_bench_symbol_and_metadata(self):
        with tempfile.TemporaryDirectory(dir=self.temp_root) as tmp:
            folder=pathlib.Path(tmp)
            (folder/'endpoints.hpp').write_text('rehab board_revision hardware_locked')
            report={'status':'passed','board':'v13','profile':'bench','bench_mode':True,
                    'real_motor_enabled':True,'motion_policy':'bounded_native_bench','source_sha256':'src'}
            symbols='''20000100 00000010 b spi1_arbiter
20000200 00000010 b spi3_arbiter
20000300 00000010 b rehab::platform::(anonymous namespace)::commands
08000100 00000010 t rehab::platform::(anonymous namespace)::bench_handle(void*)
08000120 00000010 t rehab::platform::(anonymous namespace)::network_main(void*)
08000140 00000010 T rehab::drivers::Hx711::poll(unsigned long)
08000160 00000010 V vtable for V13GateDriver
08000180 00000010 T V13GateDriver::is_ready()
080001a0 00000010 T rehab_isr_begin
080001c0 00000010 T rehab_isr_end
080001e0 00000010 T rehab_isr_max_cycles
08000200 00000010 T rehab_isr_count
'''
            def inspect(text, profile='bench'):
                (folder/'build.json').write_text(json.dumps(report))
                with patch.object(resources,'source_digest',return_value='src'), \
                        patch.object(resources.subprocess,'check_output',side_effect=['.text 512 134217728',text]):
                    return resources.inspect(profile,'v13',folder=folder)['errors']
            self.assertEqual(inspect(symbols),[])
            self.assertIn('Native bench capability/symbol mismatch',inspect(symbols.replace('::bench_handle(', '::wrong(')))
            report['bench_mode']=1
            self.assertIn('Artifact bench mode mismatch',inspect(symbols))
            report['bench_mode']=True;report['real_motor_enabled']=1
            self.assertIn('Artifact motor capability mismatch',inspect(symbols))
            report.update(profile='full',bench_mode=False,real_motor_enabled=False,motion_policy='rehab_inhibited')
            self.assertIn('Native bench capability/symbol mismatch',inspect(symbols,'full'))
            self.assertEqual(inspect(symbols.replace('::bench_handle(', '::not_bench('),'full'),[])
        with self.assertRaises(RuntimeError): resources.inspect('bench','v12')

if __name__=='__main__': unittest.main()
