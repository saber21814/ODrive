# Rehab application core checks

These executables compile the same `Rehab/core/engine.cpp` that is linked into the
firmware. They do not access a board, flash an image, or communicate with a motor.
Each source below has its own `main`; build it as a separate executable.

From the Firmware directory, using GCC 8 or newer:

```powershell
New-Item -ItemType Directory -Force build/rehab | Out-Null
g++ -std=c++17 -Wall -Wextra -Werror -pedantic -O2 Rehab/core/engine.cpp Tests/rehab/core_tests.cpp -o build/rehab/core_tests.exe
./build/rehab/core_tests.exe
g++ -std=c++17 -Wall -Wextra -Werror -pedantic -O2 Rehab/core/engine.cpp Tests/rehab/sim_main.cpp -o build/rehab/rehab_sim.exe
./build/rehab/rehab_sim.exe 60 > build/rehab/simulation.csv
g++ -std=c++17 -Wall -Wextra -Werror -pedantic -O2 -DREHAB_ENABLE_TRAINING=0 -DREHAB_ENABLE_STATISTICS=0 -DREHAB_ENABLE_COMPENSATION=0 Rehab/core/engine.cpp Tests/rehab/feature_switch_tests.cpp -o build/rehab/feature_switch_tests.exe
./build/rehab/feature_switch_tests.exe
```

Run `feature_switch_tests` for all eight 0/1 combinations of the three macros.
They default to 1 for host testing; firmware build stages provide explicit values.
The disabled compensation build rejects all compensation flags, including the
identified flag. Statistics disabled makes Fitness unsupported and RFD invalid.
Training disabled makes all `start` and `resume` calls unsupported.

`core_tests` checks configuration/ownership, directional conversion, all supported
modes, true-sample force integration, overload before filtering, stale/saturated/
invalid sensor readings, 10 SPS operation, watchdogs, timestamp wrap, pause/stop,
return limits, calibration, energy/RFD, repetition hysteresis and a fixed-seed
120 second motion sequence. Failures print the source assertion and return nonzero.

`rehab_sim [seconds]` accepts 1..3600 seconds and emits CSV at 10 Hz while stepping
the production core at 1 kHz. The deterministic model has a 4 kg moving load,
18 N s/m viscous loss, an 80 N/m passive restoring load, periodic user force, a
20 ms actuator time constant, and quantized/noisy force samples every 13 ms.
It requests a controlled stop and then checks actual transition to Standby.
The passive restoring load returns the cable; active return is disabled. This
model is a software integration check, not identification of the real machine.

## Core behavior and units

- Positive application velocity is cable payout. `direction` maps encoder/motor
  sign to payout; the constant tension torque sign is `-direction`. Position is
  `(encoder_turns-zero_turns)*direction*2*pi*radius/gear/encoder_ratio`.
- Core ownership uses a nonzero session. Platform code must first drain native
  commands, confirm IDLE and disarmed, and establish its own exclusive lease.
  `Engine` has one task owner; it contains no locks and is not an ISR API.
- `enable` grants Ready permission without arming. `start`/`resume` publishes an
  enabled target. Pause ramps torque to zero, publishes disabled, and waits for
  actual IDLE/disarmed before Paused. Normal stop ends in Standby and keeps the
  lease until an explicit release. Faults immediately publish a disabled target.
- Configuration is an atomic whole-object commit in Standby while disarmed. A
  failed commit preserves the prior configuration. A successful commit requires
  fresh sensor data before enable. Real hardware readiness is a separate platform
  condition; valid core parameters alone never authorize real board operation.
- Calibration can begin with no mechanical configuration. Tare requires eight
  stable new raw samples; calibration requires eight more stable samples at a
  known positive load, sufficient raw-count separation, and a previous tare.
  Existing valid scale is preserved by ordinary tare. Successful calibration or
  tare in Ready returns to Standby. Both invalidate old converted measurements;
  fresh force data is required before enable/resume. Missing mechanics still
  keeps `config_valid=false` after otherwise successful sensor calibration.
- No active return means negative cable velocity immediately removes commanded
  pulling torque, overriding slew rate. Measured residual actuator torque is not
  assumed to disappear instantly. Enabled return uses the independent eccentric
  target, tapers tension near speed/travel limits, and cannot brake inward motion
  by increasing pulling torque. Return speed/time interlocks also apply during
  controlled stopping. Hard force/speed/travel faults always disarm immediately.
- Force PI integrates only on fresh force samples. RFD uses an interpolated fixed
  window of real filtered force sample timestamps. Statistics count a repetition
  after a confirmed outward and inward leg and a subsequent confirmed reversal;
  minimum travel and dwell reject direction chatter. The last partial cycle does
  not count. Energy magnitudes are independently accumulated for each direction.
- All time comparisons use wrapping unsigned microseconds; supported timeout
  settings remain below half the 32-bit clock range. The application task and
  feedback must arrive before `target_timeout_ms`; the platform must separately
  enforce target expiration in the motor control path if this task stops running.

All hardware pin waveforms, current sign, real load stability, brake operation,
thermal behavior and interrupt deadlines remain subject to actual board testing.
