# Runtime integration tests

`../runtime_tests.cpp` includes the real `Rehab/platform/runtime.cpp` in its
translation unit and links the real application engine, protocol service,
framing/parser, and configuration-field code. It also includes the real Fibre
`Property::exchange` implementation, so write admission and the explicit stop
exception exercise the production property path.

The headers in this directory replace only ODrive carrier objects, HAL registers,
RTOS time/tasks/events, and the board ADC read. They reuse the driver tests'
GPIO/DWT register definitions. The test supplies `ODrive::clear_errors` with the
production hook ordering and an optional injected emergency stop. A host-only
exception from `vTaskDelayUntil` ends a single iteration of the actual infinite
safety worker, allowing assertions on its state without copying its logic.

## Build and run

Run from `Firmware` using the host `g++`, not the ARM compiler:

```powershell
$runtimeOptions = @(
  '-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-DREHAB_V12',
  '-DREHAB_ENABLE_APPLICATION=1', '-DREHAB_ENABLE_PROTOCOL=1',
  '-DREHAB_ENABLE_HX711=0', '-DREHAB_ENABLE_NETWORK=0',
  '-DREHAB_ENABLE_STATISTICS=1', '-DREHAB_ENABLE_TRAINING=1',
  '-DREHAB_ENABLE_COMPENSATION=1', '-DREHAB_STAGE=6',
  '-ITests/rehab/runtime_mocks', '-ITests/rehab/driver_mocks',
  '-IDrivers/STM32', '-Ifibre-cpp/include', '-I.',
  'Tests/rehab/runtime_tests.cpp', 'Rehab/core/engine.cpp',
  'Rehab/protocol/frame.cpp', 'Rehab/protocol/config_fields.cpp',
  'Rehab/protocol/service.cpp'
)
g++ @runtimeOptions -o build/rehab_runtime_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Runtime test compilation failed' }
& ./build/rehab_runtime_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Runtime tests failed' }

# A requested real-motor build flag cannot override unverified board declarations.
g++ @runtimeOptions -DREHAB_REAL_MOTOR=1 -o build/rehab_runtime_real_flag_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Real-flag test compilation failed' }
& ./build/rehab_runtime_real_flag_tests.exe
if ($LASTEXITCODE -ne 0) { throw 'Real-flag tests failed' }
```

Do not add `Rehab/platform/runtime.cpp` separately to the command: the test
already includes it. No production file is modified or replaced. The real
`commissioning.hpp` is used unchanged; hardware declarations remain unverified.

## Results and coverage

Both build variants currently pass **124 assertions in 16 scenarios**, with all
host compiler warnings treated as errors:

1. Hardware lock remains closed despite direct native configuration changes,
   real Fibre writes, forged valid application targets, and the real-motor flag.
2. An admitted native writer blocks training acquisition; owned training rejects
   subsequent native/Fibre writes, permits reads, and honors the stop property.
3. Pending calibration, armed state, step input, sensorless mode, incompatible
   motor/encoder selection, wrong encoder axis, and valid enabled anticogging
   cannot be taken over by training.
4. Old target epoch, expiry, and invalid limits produce the runtime fault latch;
   the precise deadline boundary is checked.
5. A stop injected during clear cannot be erased, including a new occurrence
   with the same fault bit as the pre-existing latch.
6. Successful clear synchronizes target/owner epochs and does not re-enable.
7. Native clear followed by one real worker iteration does not relatch an old
   application fault; a new hardware latch remains sticky.
8. A single USB/UART request schedules event 8/4 and produces a complete response;
   a repeated active-session RNG nonce is refused before accepting a new nonce.
9. The post-controller hook clamps excessive magnitude and reversed direction,
   preserves a reduced native torque, zeros inactive outputs, rejects missing/
   nonfinite output, and leaves unowned Native control unchanged.
10. A disconnect discards pending old-session output before a fresh HELLO.
11. Empty worker polls do not extend a partial-frame timeout; parsing recovers.
12. An oversized RX enqueue increments overflow diagnostics and trips owned motion.
13. The real `Property::diagnostic_argument()` copy accepts a read-only RPC's
    staging argument during training ownership; the original property and
    unrelated native inputs remain guarded, and reads remain available.
14. Fault clear followed by release zeros native input torque, input velocity,
    and torque setpoint before returning ownership. Release is checked without
    relying on an intervening control ISR to clear stale inputs.
15. A negotiated observer's protocol STOP immediately invokes the actual runtime
    disarm/latch hook even while the motor belongs to native control.
16. Protocol Finish rejects an observer, lets the owner enter controlled stopping,
    and reduces the real engine target within its configured slew rate. Normal
    release clears residual inputs, and a subsequent disconnect adds no fault.

## Limits

These tests do not prove physical STOP polarity, electrical disarming, sensor
waveforms, USB/UART DMA completion, actual FreeRTOS scheduling/preemption, MCU
instruction timing, or FOC behavior. STOP injection tests the runtime latch,
not the GPIO-to-EXTI electrical path: the production STOP polarity is deliberately
unconfigured. The minimal `Motor::disarm` carrier records software disarming;
it does not emulate the power stage. Full generated Fibre function stubs,
ASCII/CAN dispatch, and board ISR hookup need their separate structural/build
checks. Network and HX711 hardware behavior is covered by separate driver-model
tests and remains subject to real-board acceptance.
