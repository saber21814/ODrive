"""Check exact native-source preservation and fail-closed observation hooks."""
import importlib.util
from pathlib import Path
import unittest
import shutil
import subprocess

FW = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('instrument_calibration', FW/'Rehab/scripts/instrument_calibration.py')
instrument = importlib.util.module_from_spec(spec)
spec.loader.exec_module(instrument)


class InstrumentationTests(unittest.TestCase):
    def setUp(self):
        self.source = (FW/'MotorControl/motor.cpp').read_bytes()

    def test_exact_native_bytes_preserved(self):
        output = instrument.instrument(self.source)
        self.assertEqual(instrument.strip_instrumentation(output), self.source)
        self.assertEqual(len(instrument.BLOCK.findall(output)), 5)
        self.assertEqual((FW/'MotorControl/motor.cpp').read_bytes(), self.source)

    def test_line_endings_preserved(self):
        for source in (self.source.replace(b'\r\n', b'\n'), self.source.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')):
            self.assertEqual(instrument.strip_instrumentation(instrument.instrument(source)), source)

    def test_missing_anchor_fails(self):
        with self.assertRaises(ValueError):
            instrument.instrument(self.source.replace(b'test_mod_ = test_voltage_ * vfactor;', b'test_mod_ = 0;'))

    def test_duplicate_anchor_fails(self):
        newline = b'\r\n' if b'\r\n' in self.source else b'\n'
        with self.assertRaises(ValueError):
            instrument.instrument(self.source + b'        *ibus = 0.0f;' + newline)

    def test_cannot_instrument_twice(self):
        with self.assertRaises(ValueError):
            instrument.instrument(instrument.instrument(self.source))

    def test_hook_order_and_phase(self):
        output = instrument.instrument(self.source).decode()
        resistance, inductance = output.split('struct InductanceMeasurementControlLaw', 1)
        self.assertLess(resistance.index('test_mod_ = test_voltage_ * vfactor;'), resistance.index('model_measure(1'))
        self.assertLess(resistance.index('*ibus = *test_mod_ * actual_current_;'), resistance.index('model_output(1'))
        self.assertLess(inductance.index('last_input_timestamp_ = input_timestamp;'), inductance.index('model_measure(2'))
        self.assertLess(inductance.index('*ibus = 0.0f;'), inductance.index('model_output(2'))
        self.assertIn('model_measure(1, NAN, NAN, test_voltage_, NAN, false)', resistance)
        self.assertIn('**ibus, output_timestamp, true', resistance)
        self.assertIn('**ibus, output_timestamp, true', inductance)

    def test_build_is_v13_only(self):
        tup = (FW/'Tupfile.lua').read_text()
        selection = tup.split("-- V1.3-only generated observation copy", 1)[0]
        self.assertTrue(selection.rstrip().endswith("if boardversion == 'rehab-v1.3' then"))
        rule = tup.split("tup.frule{inputs={'MotorControl/motor.cpp'", 1)[0]
        self.assertTrue(rule.rstrip().endswith("if boardversion == 'rehab-v1.3' then"))
        self.assertIn("odrive_firmware_pkg.code_files[i] = 'autogen/motor_v13_capture.cpp'", tup)

    def test_native_and_instrumented_numeric_behavior(self):
        def classes(source):
            text = source.decode()
            return text[text.index('struct ResistanceMeasurementControlLaw'):text.index('Motor::Motor(')]
        harness = r'''
#include <optional>
#include <utility>
#include <cmath>
#include <cassert>
#include <iostream>
using float2D = std::pair<float,float>;
constexpr float current_meas_period=0.000125f;
constexpr float TIM_1_8_CLOCK_HZ=168000000.0f;
float vbus_voltage=24.0f;
struct ODriveIntf { struct MotorIntf { enum Error {
 ERROR_NONE,ERROR_PHASE_RESISTANCE_OUT_OF_RANGE,ERROR_UNKNOWN_VBUS_VOLTAGE,
 ERROR_CONTROLLER_INITIALIZING,ERROR_UNKNOWN_CURRENT_MEASUREMENT
}; }; };
using Error=ODriveIntf::MotorIntf::Error;
struct Motor:ODriveIntf::MotorIntf {};
struct AlphaBetaFrameController {
 virtual void reset()=0;
 virtual Error on_measurement(std::optional<float>,std::optional<float2D>,uint32_t)=0;
 virtual Error get_alpha_beta_output(uint32_t,std::optional<float2D>*,std::optional<float>*)=0;
};
namespace rehab { namespace capture {
 bool active=false; unsigned measurements=0,outputs=0; float observed=0;
 void model_measure(unsigned,float a,float b,float v,float i,bool valid) {
   if (active) { ++measurements; observed=valid ? a+b+v+i : NAN; }
 }
 void model_output(unsigned,float v,float i,uint32_t,bool valid) {
   if (active) { ++outputs; observed=valid ? v+i : NAN; }
 }
}}
namespace native { NATIVE_CLASSES }
namespace diagnostic { DIAGNOSTIC_CLASSES }
void equal(float a,float b) { assert(a==b || (std::isnan(a)&&std::isnan(b))); }
template<typename A,typename B>
void measurement(A& a,B& b,std::optional<float> v,std::optional<float2D> i,uint32_t t) {
 assert(a.on_measurement(v,i,t)==b.on_measurement(v,i,t));
 equal(a.test_voltage_,b.test_voltage_);
}
template<typename A,typename B>
void output(A& a,B& b,uint32_t t) {
 std::optional<float2D> ma,mb; std::optional<float> ia,ib;
 assert(a.get_alpha_beta_output(t,&ma,&ia)==b.get_alpha_beta_output(t,&mb,&ib));
 assert(bool(ma)==bool(mb) && bool(ia)==bool(ib));
 if(ma) { equal(ma->first,mb->first); equal(ma->second,mb->second); }
 if(ia) equal(*ia,*ib);
 equal(a.test_voltage_,b.test_voltage_);
}
int main() {
 for(bool enabled: {false,true}) {
  rehab::capture::active=enabled;
  native::ResistanceMeasurementControlLaw a;
  diagnostic::ResistanceMeasurementControlLaw b;
  a.target_current_=b.target_current_=0.5f;
  a.max_voltage_=b.max_voltage_=1.0f;
  a.reset(); b.reset(); output(a,b,0);
  for(unsigned j=1;j<=300;j++) {
   measurement(a,b,24.0f,float2D{j<150 ? 0.4f : -0.7f,0.02f},j*21000);
   output(a,b,j*21000+10000);
   equal(a.get_resistance(),b.get_resistance()); equal(a.get_Ibeta(),b.get_Ibeta());
  }
  measurement(a,b,24.0f,std::nullopt,7000000); output(a,b,7010000);
  measurement(a,b,std::nullopt,float2D{0.2f,0.01f},7020000);
  measurement(a,b,24.0f,float2D{NAN,NAN},7030000); output(a,b,7040000);
  a.reset(); b.reset(); a.max_voltage_=b.max_voltage_=0.00001f;
  measurement(a,b,24.0f,float2D{0.0f,0.0f},7050000);
  equal(a.get_resistance(),b.get_resistance());
  native::InductanceMeasurementControlLaw c;
  diagnostic::InductanceMeasurementControlLaw d;
  c.reset(); d.reset(); c.test_voltage_=d.test_voltage_=0.5f;
  for(unsigned j=1;j<=20;j++) {
   measurement(c,d,24.0f,float2D{j%2 ? 0.1f : -0.2f,0.0f},j*21000);
   output(c,d,j*21000+10000);
   equal(c.get_inductance(),d.get_inductance());
  }
  measurement(c,d,24.0f,std::nullopt,500000);
  if(!enabled) assert(rehab::capture::measurements==0 && rehab::capture::outputs==0);
 }
 assert(rehab::capture::measurements>0 && rehab::capture::outputs>0);
 std::cout << "PASS: native/instrumented calibration outputs equal, capture off/on\n";
}
'''.replace('NATIVE_CLASSES', classes(self.source)).replace('DIAGNOSTIC_CLASSES', classes(instrument.instrument(self.source)))
        out = FW/'build/rehab/calibration_instrument_test'
        out.mkdir(parents=True, exist_ok=True)
        (out/'test.cpp').write_text(harness)
        subprocess.run([shutil.which('g++') or 'g++', '-std=c++17', '-O0', str(out/'test.cpp'), '-o', str(out/'test.exe')], check=True)
        subprocess.run([str(out/'test.exe')], check=True)


if __name__ == '__main__':
    unittest.main()
