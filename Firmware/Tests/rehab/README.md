# Rehab tests

Run the complete host matrix from `Firmware`:

```powershell
python -B Rehab/scripts/test.py
```

The script compiles the production C++ sources used by the MCU. HAL/RTOS/device
models provide deterministic inputs, not replacement control algorithms.
Results, compiler/run commands, exit codes and logs are in `build/rehab/tests/`.
`--quick` skips the 24-hour virtual run and the upstream native suite.

- [Core and CLI simulation](README_core.md)
- [Runtime adapter model](runtime_mocks/README.md)
- [SPI/HX711/CH394Q device models](../../Rehab/drivers/README.md)
- [Verification and limits](../../Rehab/docs/verification.md)

Every test executable has a separate main. No script flashes or accesses a board.
The original `step_cb` test's approximately 8.6-billion-iteration case is explicitly
excluded from the upstream suite; it must not be counted as a passing test.
