# V1.3 原生命令与基础外设

默认构建为现有 `peripherals`：V1.3 单轴硬件适配、USB Fibre、Rehab基础协议、CH394网口、HX711和温度采集；应用与训练控制关闭。网络地址192.168.11.2/24、网关0、TCP端口39400，电脑沿用192.168.11.10。

在Firmware目录执行：

```text
make
make flash-check
make flash
```

`flash-check`只构建并检查固件，不启动烧录器。`make flash`写入`build/rehab/v13/peripherals/ODriveFirmware.elf`并校验。默认固件具备原生命令使能能力，启动保持IDLE，不自动校准或运动。需要明确构建锁定full时使用`python -B Rehab/scripts/project.py build --profile full`。原板入口仍为`make -f Makefile`，不能将原板固件烧到V1.3。

台架阶段许可、台架CLI及台架构建已退役。命令22～26不复用，返回Unsupported。后续电机测试直接在odrivetool `In [...]`窗口通过`dev0`原生接口进行。

固件不替用户固定电机极对数、CPR、R/L、转矩常数、电流、校准电压或24V测试阈值。读取出的配置值不是测量结果。新电机须重新测R/L，不使用旧电机通过标志；无编码器时极对数仍未确认，电角速度不能冒充机械转速或推杆速度。

首次连接只读：

```python
print("board:", hex(dev0.rehab.board_revision))
print("stage:", dev0.rehab.stage)
print("axis_state:", dev0.axis0.current_state)
print("armed:", dev0.axis0.motor.is_armed)
print("gate_ready:", dev0.rehab.gate_ready)
print("STOP:", dev0.rehab.physical_stop)
print("VBUS:", dev0.vbus_voltage)
print("UV:", dev0.config.dc_bus_undervoltage_trip_level)
print("OV:", dev0.config.dc_bus_overvoltage_trip_level)
print("motor bus limits:", dev0.axis0.motor.config.I_bus_hard_min,
      dev0.axis0.motor.config.I_bus_hard_max)
print("system bus limits:", dev0.config.dc_max_negative_current,
      dev0.config.dc_max_positive_current)
dump_errors(dev0)
```

默认电机级母线限值恢复原生负无穷/正无穷，系统级负向母线限值仍为原生-0.01A。保护参数必须按实际供电、制动和电机确认后由命令显式设置并读回。不得用清错循环、无限阈值或旧的预校准标志掩盖故障。没有有效过压等参数时先配置再校准，不能以“原生默认”代替本机参数确认。

原生短时开环可使用LOCKIN_SPIN公开配置与状态请求；它仍有原生校准、方向和硬件就绪前提。每次运行采用单独明确的命令，并以IDLE退出。USB可通过`dev0.axis0.requested_state = 1`请求停止。网络基础查询不会持有电机控制权；TCP断开不代表原生运动看门狗，若需要断连停机应使用原生看门狗并按其控制源维护。

物理STOP、DRV恢复、ADC映射、双SPI隔离等板级功能保留。保留诊断不表示已完成实机急停、采样准确度、制动回生或行程保护验收。无编码器和限位的推杆不能自动判断行程端点。


## Temporary V1.3 calibration capture (2026-11-04)

This observer is OFF after boot. `dev0.rehab.cal_capture_arm()` returns 0 only
while axis0 is IDLE, no state request is queued and motor PWM is not armed.
It only prepares RAM recording; it never clears faults or starts calibration.
Arm explicitly before ONE independently configured native calibration attempt.
Do not rerun calibration automatically if no capture is available.

`cal_capture_word(index)` reads metadata or frozen records only. Clear-errors
and motor disarm do not erase the capture. Explicit re-arm replaces it; power
loss erases RAM. No persistence or network command IDs were added.

The first finite pre-brake model Ibus < -0.01 A freezes the most recent 64
samples, including that sample. Threshold is diagnostic only; no protection is
changed. If a native error arrives first, reason 2 freezes it. Calibration end
without either event freezes reason 3. Thus current-limit faults can be captured
without weakening current protection to force a negative-current event.

Run the read-only exporter after axis0 returns to IDLE:

```python
import runpy
_ = runpy.run_path(
    r"D:\00-ODrive-text\ODrive-fw-0.5.6\Firmware\Rehab\scripts\read_calibration_capture.py",
    init_globals={"dev0": dev0}, run_name="__main__")
```

Files go to `Firmware/build/rehab/calibration_captures/` as JSON (raw words
included) and CSV. Do not interpret absent/NaN model values as zero.

Word metadata 0..8: version=1, state(0 disabled/1 recording/2 frozen), generation,
count, reason(1 negative/2 native error/3 end), trigger row (UINT_MAX for end),
capacity=64, record length=28, threshold float bits. Data starts at word 16,
chronological rows of 28 words; floating fields 8..18 are IEEE754 float32.

Record columns: sequence, sample timestamp, phase(0 unknown/1 resistance/2
inductance), flags, ADC2 JDR1, ADC3 JDR1, ADC2 SR, ADC3 SR, Ialpha, Ibeta,
test_voltage, estimated_ibus, Vbus, DC_A, DC_B, DC_C, inverse_gain, current_trip,
runtime_ibus_after_PWM_callback, motor_error_low, motor_error_high, system_error,
BDTR_at_sample, BDTR_after, armed_state_at_sample, output_timestamp, reserved,
reserved. Flags: 1 raw conversion valid; 2 corrected current valid; 4 model
estimate valid; 8 MOE at sample; 16 software armed at sample; 32 model output
produced. All values correspond to one control ISR processing cycle; sample and
output timestamps deliberately differ because the native loop schedules future
PWM. Invalid raw samples must not be interpreted as valid ADC measurements.

Interpretation limits:
- Test voltage is the model COMMAND for the output timestamp, not measured
  terminal voltage. Output-valid does not prove SVM/PWM was successfully applied.
- In inductance calibration native Ibus is fixed at 0; this is not measured
  absence of regeneration. The output voltage changes sign after measurement.
- A current-limit fault can run before the model callback. That row retains
  corrected currents/raw ADC but marks unavailable model values invalid/NaN.
- Samples retain the pre-compensation DC values before independent DC sampling.
- This can distinguish sustained sign inconsistency from isolated sample spikes;
  it cannot independently prove physical phase-current direction.
- Compare existing `rehab.isr_control_max_cycles` before/after an armed capture.
  Host tests and the ARM build do not certify real-target ISR deadline margin.

MotorControl source is unmodified. Only V1.3 builds generate an insertion-only
motor observation copy. Removing the marked observer blocks exactly restores
the original source bytes. Removing this temporary feature means removing the
V1.3 build selection, board hooks and appended capture APIs, not editing native
FOC or calibration algorithms.
