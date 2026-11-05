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

class ProjectProfilesTests(unittest.TestCase):
    def setUp(self):
        self.temp_root=FW/'build/rehab/tests'
        self.temp_root.mkdir(parents=True,exist_ok=True)

    def test_retired_profile_rejected_before_writing(self):
        with patch.object(build, 'OUT') as out:
            with self.assertRaises(RuntimeError): build.build('bench',board='v12')
            for profile in ('native','hx711','network','protection','training','statistics','full'):
                with self.assertRaises(RuntimeError): build.build(profile,board='v13')
            out.mkdir.assert_not_called()

    def test_profile_isolation_report_and_config_restore(self):
        for profile,board in [('peripherals','v13'),('peripherals','v12'),('full','v12'),('baseline','v12')]:
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
                native=profile=='peripherals' and board=='v13'
                self.assertNotIn('CONFIG_REHAB_BENCH=',config)
                if profile!='baseline': self.assertIn('CONFIG_REHAB_REAL_MOTOR=false',config)
                if profile=='peripherals':
                    for option in ('HX711','NETWORK','PROTOCOL'): self.assertIn('CONFIG_REHAB_'+option+'=true',config)
                    for option in ('APPLICATION','TRAINING','STATISTICS','COMPENSATION'): self.assertIn('CONFIG_REHAB_'+option+'=false',config)
                    self.assertIn('CONFIG_REHAB_STAGE=2',config)
                if profile=='full' or native: capture.assert_called_once_with(board,profile)
                else: capture.assert_not_called()
                folder=(out/'v13' if board=='v13' and profile!='baseline' else out)/profile
                report=json.loads((folder/'build.json').read_text())
                self.assertIs(report['native_control'],native)
                self.assertNotIn('bench_mode',report)
                self.assertIs(report['real_motor_enabled'],None if profile=='baseline' else native)
                self.assertEqual(report['motion_policy'], 'original_board_native' if profile=='baseline' else
                                 'native_board_control' if native else 'rehab_inhibited')

    def test_resource_profile_and_metadata(self):
        with tempfile.TemporaryDirectory(dir=self.temp_root) as tmp:
            folder=pathlib.Path(tmp)
            (folder/'endpoints.hpp').write_text('rehab board_revision hardware_locked')
            (folder/'ODriveFirmware.map').write_text('build/obj/_MotorControl_motorcpp.o')
            report={'status':'passed','board':'v13','profile':'peripherals','native_control':True,
                    'real_motor_enabled':True,'motion_policy':'native_board_control','source_sha256':'src'}
            symbols='''20000100 00000010 b spi1_arbiter
20000200 00000010 b spi3_arbiter
20000300 00000010 b rehab::platform::(anonymous namespace)::commands
08000120 00000010 t rehab::platform::(anonymous namespace)::network_main(void*)
08000140 00000010 T rehab::drivers::Hx711::poll(unsigned long)
08000160 00000010 V vtable for V13GateDriver
08000180 00000010 T V13GateDriver::is_ready()
'''
            def inspect(text, profile='peripherals'):
                (folder/'build.json').write_text(json.dumps(report))
                with patch.object(resources,'source_digest',return_value='src'), \
                        patch.object(resources.subprocess,'check_output',side_effect=['.text 512 134217728',text]):
                    return resources.inspect(profile,'v13',folder=folder)['errors']
            self.assertEqual(inspect(symbols),[])
            report['native_control']=1
            self.assertIn('Artifact native-control mode mismatch',inspect(symbols))
            report['native_control']=True;report['real_motor_enabled']=1
            self.assertIn('Artifact motor capability mismatch',inspect(symbols))
            report.update(profile='full',native_control=False,real_motor_enabled=False,motion_policy='rehab_inhibited')
            with self.assertRaises(RuntimeError): inspect(symbols,'full')
            report.update(profile='peripherals', native_control=True, real_motor_enabled=True, motion_policy='native_board_control')
            for retired in ('rehab::Engine::update', 'rehab::capture::recorder', 'rehab_board_capture_arm', 'rehab_isr_begin'):
                self.assertTrue(inspect(symbols+'08000200 00000010 T '+retired+'\n'))
            (folder/'ODriveFirmware.map').write_text('autogen/motor_v13_capture.cpp')
            self.assertIn('Original motor.cpp object missing from V1.3 link map', inspect(symbols))
        with self.assertRaises(RuntimeError): resources.inspect('bench','v12')

    def test_editor_profile_contract(self):
        import sqlite3
        for profile in ('peripherals',):
            with tempfile.TemporaryDirectory(dir=self.temp_root) as tmp:
                root=pathlib.Path(tmp); (root/'.tup').mkdir(); (root/'autogen').mkdir()
                for name in editor.HEADER_NAMES:
                    (root/'autogen'/name).write_text('RehabStatusIntf '+profile)
                flags=['-DREHAB_V12=1','-DREHAB_V13=1','-DREHAB_STAGE='+str(6 if profile=='full' else 2)]
                flags+=['-DREHAB_ENABLE_'+n+'=1' for n in ('HX711','NETWORK','PROTOCOL')]
                flags+=['-DREHAB_ENABLE_'+n+'='+str(int(profile=='full')) for n in ('APPLICATION','TRAINING','STATISTICS','COMPENSATION')]
                with sqlite3.connect(root/'.tup/db') as db:
                    db.execute('CREATE TABLE node (name TEXT,type INTEGER)')
                    db.execute('INSERT INTO node VALUES (?,1)', ('arm-none-eabi-g++ -c MotorControl/main.cpp '+' '.join(flags),))
                db.close()
                with patch.object(editor,'ROOT',root),patch.object(editor.shutil,'which',side_effect=lambda n: 'C:/tool/g++.exe' if n.startswith('arm-') else None),contextlib.redirect_stdout(io.StringIO()):
                    editor.capture('v13',profile)
                    context=root/('build/rehab/v13/editor' if profile=='full' else 'build/rehab/v13/peripherals/editor')
                    self.assertEqual(json.loads((context/'context.json').read_text())['profile'],profile)
                    with self.assertRaises(RuntimeError): editor.capture('v13','peripherals' if profile=='full' else 'full')

if __name__=='__main__': unittest.main()
