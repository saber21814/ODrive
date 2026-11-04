"""Execute extracted production sample/limit logic without MCU or motor access."""
from pathlib import Path
import subprocess
import shutil

FW = Path(__file__).resolve().parents[2]
board = (FW / 'Board/rehab_v13/board.cpp').read_text()
motor = (FW / 'MotorControl/motor.cpp').read_text()
adapter = board.split('    if (!(TIM1->BDTR & TIM_BDTR_MOE_Msk)) {', 1)[1].split('\n    }', 1)[0]
body = motor.split('void Motor::current_meas_cb(', 1)[1]
body = body[body.index('    bool dc_calib_valid'):body.index('    if (control_law_)')]
out = FW / 'build/rehab/idle_current_test'
out.mkdir(parents=True, exist_ok=True)
source = r'''
#include <optional>
#include <cmath>
#include <cassert>
#include <iostream>
using std::abs;
#define SQ(x) ((x)*(x))
struct ABC { float phA, phB, phC; };
struct { void do_fast_checks() {} } odrv;
enum { ERROR_CURRENT_LIMIT_VIOLATION=1, ERROR_UNKNOWN_CURRENT_MEASUREMENT=2 };
struct Motor {
  struct { float dc_calib_tau=1, current_lim_margin=.5f; } config_;
  float dc_calib_running_since_=8, max_dc_calib_=6, effective_current_lim_=1.5f;
  ABC DC_calib_={2.955f,-1.688f,-1.267f};
  int armed_state_=0, error=0; bool is_armed_=false;
  std::optional<ABC> current_meas_;
  void disarm_with_error(int e) { error |= e; is_armed_=false; armed_state_=0; }
  void sample(std::optional<ABC> current) { BODY }
} motors[1];
void feed(bool moe, std::optional<ABC> current0) {
  if (!moe) { ADAPTER }
  motors[0].sample(current0);
}
void zero() {
  auto c=motors[0].current_meas_; assert(c && c->phA==0 && c->phB==0 && c->phC==0);
}
int main() {
  auto& m=motors[0];
  // Original raw-zero path reproduces the user's false trip.
  m.sample(ABC{0,0,0}); assert(m.error==ERROR_CURRENT_LIMIT_VIOLATION);
  m=Motor{}; feed(false,ABC{9,9,9}); zero(); assert(!m.error);
  // Invalid DC calibration remains invalid, even with synthetic samples.
  m.dc_calib_running_since_=0; feed(false,ABC{0,0,0}); assert(!m.current_meas_);
  m.is_armed_=true; feed(false,std::nullopt); assert(m.error==ERROR_UNKNOWN_CURRENT_MEASUREMENT);
  // Preserve native two-sample arming startup, then real ADC compensation.
  m=Motor{}; m.is_armed_=true; m.armed_state_=1;
  feed(false,std::nullopt); zero(); assert(m.armed_state_==2);
  feed(true,ABC{9,9,9}); zero(); assert(m.armed_state_==3);
  auto dc=m.DC_calib_;
  feed(true,ABC{dc.phA+1,dc.phB-.5f,dc.phC-.5f});
  assert(abs(m.current_meas_->phA-1)<1e-6f && !m.error);
  // Real overcurrent still trips. Following PWM shutdown cannot reintroduce bias.
  feed(true,ABC{dc.phA+3,dc.phB-1.5f,dc.phC-1.5f});
  assert(m.error==ERROR_CURRENT_LIMIT_VIOLATION && !m.is_armed_);
  feed(false,std::nullopt); zero(); assert(m.error==ERROR_CURRENT_LIMIT_VIOLATION);
  m.error=0; m.is_armed_=true; m.armed_state_=1;
  feed(false,std::nullopt); zero(); feed(true,dc); zero(); feed(true,dc); zero();
  assert(!m.error);
  std::cout << "PASS: idle regression, invalid DC, startup, real samples, overcurrent, shutdown/rearm\n";
}
'''.replace('BODY', body).replace('ADAPTER', adapter)
# The independent DC-calibration conversion must still overwrite current0.
tail = board.split('motors[0].current_meas_cb(', 1)[1]
assert tail.index('fetch_and_reset_adcs(&current0, true)') < tail.index('motors[0].dc_calib_cb(')
assert tail.index('current0.reset();') < tail.index('fetch_and_reset_adcs(&current0, true)')
(out / 'test.cpp').write_text(source)
subprocess.run([shutil.which('g++') or 'g++', '-std=c++17', '-O2', str(out/'test.cpp'), '-o', str(out/'test.exe')], check=True)
subprocess.run([str(out/'test.exe')], check=True)
