# Native ingress production-source regression

This harness executes complete functions taken directly from firmware sources,
the complete actual generated `interfaces.hpp`/`function_stubs.hpp`, real Fibre
`Property` and type conversions, and the entire `Rehab/platform/runtime.cpp`.
No admission or dispatch implementation is copied into a handwritten substitute.

Run from `Firmware`. The first command invokes the real V1.2 schema script and
the real Fibre generator with the same templates/options as Tup. Its four
headers and schema live under `build/rehab/ingress_generated/autogen`, independent
of the board last built by ARM/Tup. It never writes or reads `Firmware/autogen`.

```powershell
python -B Rehab/scripts/ingress_sources.py --output build/rehab/ingress_generated
g++ -std=gnu++17 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -DREHAB_V12=1 -DREHAB_ENABLE_APPLICATION=1 -DREHAB_ENABLE_PROTOCOL=1 -DREHAB_ENABLE_HX711=0 -DREHAB_ENABLE_NETWORK=0 -DREHAB_ENABLE_TRAINING=1 -DREHAB_ENABLE_STATISTICS=1 -DREHAB_ENABLE_COMPENSATION=1 -DREHAB_STAGE=6 -ITests/rehab/ingress/mocks -ITests/rehab/driver_mocks -IDrivers/STM32 -Ifibre-cpp/include -Ibuild/rehab/ingress_generated -I. Tests/rehab/ingress_tests.cpp Rehab/core/engine.cpp Rehab/protocol/frame.cpp Rehab/protocol/config_fields.cpp Rehab/protocol/service.cpp Rehab/platform/isr_timing.cpp -o build/rehab/ingress_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Ingress compilation failed' }
./build/rehab/ingress_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Ingress tests failed' }
```

Source preparation is a static operation and requires the existing Python
generator dependencies, not an ARM compiler. After generation, `--check` validates
the independent artifacts without writing files. It verifies firmware Tup source membership, relevant
caller tokens, V1.2 generated root, and exactly four diagnostic-argument cases.
Each complete function is emitted unchanged, with its original `#line`, full
source SHA256, function SHA256 and line span in `manifest.json`. Generated headers
and runtime are whole-file inputs and have their SHA256 recorded. The manifest
also records the schema script, original schema, generator stub, actual generator,
four templates, generator command argument arrays and every generated artifact's
SHA256. `--check` rejects altered generator inputs, generated headers or extracted
bodies. Regenerate on each test invocation so an old extracted body cannot hide
a source change. Keep `-Ibuild/rehab/ingress_generated` before `-I.` so quoted
`autogen/...` includes select these independent generated headers.

The extraction boundary avoids compiling unrelated STM32 initialization and
FreeRTOS implementations. ASCII `process_line`, all command handlers and the
actual buffered response formatter are executed. CAN node routing, `do_command`
and all callbacks are executed; transport subscription/service scheduling is
outside the harness. HAL/RTOS, hardware carrier state, CAN bus and stream sink
are mocked. Mock motor methods count software disarming; no real motor runs.

The minimal property directory exposes three real Fibre properties. The complete
generated RPC/property stubs and `set_endpoint_from_float` are real code; the full
generated endpoint dispatcher/tree is not executed. Its diagnostic-argument
whitelist and build generation are checked structurally. PWM enters complete
`handle_pulse`, analog enters complete `update_analog_endpoint`, and both invoke
the actual endpoint write function. GPIO capture-to-handler wiring is a static
call-chain check, not a mock electrical waveform test.

The test matrix covers Native, actual Engine Running and actual Engine Paused.
The Engine transitions are exercised directly without changing the production
hardware lock; carrier motors remain idle/disarmed. It checks CAN reads and all
implemented write dispatch cases, ASCII reads/motion/property/system commands,
Fibre reads including staged parameters, motion/calibration/clear/save/reboot
RPCs, IDLE/ESTOP exceptions, and native writes after clear/release. Actual runtime
`axis_request_allowed` is called; the full axis RTOS state-machine loop is not
simulated. Clearing/releasing must not energize the carrier.

Step tests execute actual `Axis::step_cb` and `set_step_dir_active`: admission,
late edges after handoff, subscription failure, release then restart, positive
and negative increments, boundaries around 2^24/2^33/2^53 and int64 limits, plus
10,000 deterministic xorshift samples. Integer step counts remain exact where a
float can no longer distinguish one step. The original test's 1/8192 scale is
checked with a long-double reference projection; full `Controller::update` is
not replaced or claimed covered. This bounded regression supplements the
excluded original 2^33-iteration test; it does not report that test as executed.

No source preparation or host execution proves physical STOP polarity, NVIC
latency, task preemption, USB/CAN throughput, ADC/PWM timing or FOC behavior.
Compilation/execution results must be supplied by the integrating test runner;
this document does not claim they have already passed.
