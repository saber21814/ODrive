# ODrive 3.6 + AS5600 + 4015/33 W 配置命令建议

> 适用对象：ODrive 3.6（0.5.6 改造固件）、Motor 1/Axis 1、24 V 4015 外转子无刷电机、AS5600 I²C 模块。本文命令依赖本项目新增的 `ENCODER_MODE_I2C_ABS_AS5600 = 0x105`，原版 0.5.6 固件不能仅靠命令启用该编码器。

## 1. 硬件和安全前提

| AS5600 模块 | ODrive 3.6 J4 | 说明 |
|---|---|---|
| VCC | Pin 2 / 3.3 V | 模块必须由 3.3 V 供电；确认模块没有把 SDA/SCL 上拉到 5 V |
| SCL | Pin 3 / M1_ENC_A / PB6（GPIO12） | I²C1 SCL |
| SDA | Pin 4 / M1_ENC_B / PB7（GPIO13） | I²C1 SDA |
| GND | Pin 6 | 共地 |
| OUT | 不接 | AS5600 PWM/模拟输出不用于本改造 |

按 J4 和模块丝印复核针脚方向。I²C 地址固定为 `0x36`；模块必须有 SDA、SCL 上拉，且上拉电压不能超过 3.3 V。AS5600 的磁铁应为安装在转子轴上的单对极 diametric 磁铁，气隙和同心度按 AS5600 数据手册要求调整。

4015 电机资料参数：24 V、33 W、1000 rpm、1.4 A、最大/堵转约 4.9 A、24N22P（11 极对）、三角形绕组。厂家相电阻/电感约 5.4 Ω/1.3 mH（实测应允许偏差）。

## 2. 刷写前备份与刷写后初始化

先在仍运行旧固件时执行：

```python
odrivetool backup-config odrive_backup.json
```

保存备份文件，但不要在新固件上直接恢复旧编码器的 `phase_offset`、`phase_offset_float`、`direction` 或 `pre_calibrated`。这些量必须在 AS5600 和磁铁安装确认后重新校准。

刷入使用本工程构建的 v3.6-56V 固件（DFU/官方刷写流程均可）。刷写后重新连接并执行下列配置。若设备仍是原版固件，设置 `mode=0x105` 会报不支持，必须先刷改造固件。

## 3. AS5600 编码器配置（改造固件）

在 `odrivetool` Python 交互窗口中：

```python
from odrive.enums import *

odrv0.config.enable_i2c_a = False       # 禁用旧 I2C 从机服务
odrv0.config.gpio12_mode = GPIO_MODE_I2C_A
odrv0.config.gpio13_mode = GPIO_MODE_I2C_A

enc = odrv0.axis1.encoder.config
enc.mode = ENCODER_MODE_I2C_ABS_AS5600  # 0x105
enc.cpr = 4096
enc.use_index = False
enc.bandwidth = 300
enc.pre_calibrated = False
enc.phase_delay_compensation = 0.0004   # s，允许范围 0..0.005

odrv0.axis1.motor.config.motor_type = MOTOR_TYPE_HIGH_CURRENT
odrv0.axis1.motor.config.pole_pairs = 11
odrv0.axis1.motor.config.current_lim = 1.5
odrv0.axis1.motor.config.current_lim_margin = 1.0
odrv0.axis1.motor.config.requested_current_range = 5.0
odrv0.axis1.motor.config.calibration_current = 0.8
odrv0.axis1.motor.config.resistance_calib_max_voltage = 6.0
odrv0.axis1.motor.config.current_control_bandwidth = 500
odrv0.axis1.motor.config.torque_constant = 8.27 / 54.0

odrv0.axis1.controller.config.vel_limit = 2.0
odrv0.axis1.controller.config.vel_gain = 0.03
odrv0.axis1.controller.config.vel_integrator_gain = 0.1
```

`8.27/54 ≈ 0.153 N·m/A` 是 ODrive 按 KV 换算的初始值。厂家给出 `0.23 N·m/A`，两者可能采用了不同的电流定义（相电流/线电流或峰值/RMS）；首次调试不要直接照搬 0.23。

配置完成后保存并重启，使 GPIO 复用生效：

```python
odrv0.save_configuration()
odrv0.reboot()
```

设备重连后重新取得对象：

```python
odrv0 = odrive.find_any()
```

固件启动时会对 AS5600 的 `CONF(0x07/0x08)` 做易失读改写：正常功耗、watchdog 关闭、`SF=11`、低阈值快速滤波；只读回验证，不写 `OTP 0xFF`。运行时每约 250 µs 发起一次角度读取（RAW ANGLE 0x0C/0x0D），状态读取约 1 kHz。

## 4. 必须按顺序执行的校准

1. 只接逻辑电源时，用下面命令读取诊断量。缓慢转轴一圈并反向转回，观察 `pos_abs`/`shadow_count`：一机械圈必须对应一次 0..4095 完整周期，`shadow_count` 跨零时应连续增减。若一圈重复多次，原电机磁环不是 AS5600 所需的单对极磁铁，停止后续校准。

```python
enc1 = odrv0.axis1.encoder
print(enc1.as5600_status, enc1.i2c_error_rate, enc1.sample_age)
print(enc1.pos_abs, enc1.shadow_count, enc1.error)
dump_errors(odrv0)
```
2. 确认 `sample_age < 0.002`、I²C 错误率接近 0，且没有 `ABS_I2C_COM_FAIL`/`ABS_I2C_MAGNET_ERROR`。
3. 功率级接 24 V 限流电源，首次空载或轻载。执行电机校准：

```python
odrv0.axis1.requested_state = AXIS_STATE_MOTOR_CALIBRATION
```

4. 无错误后执行编码器偏置校准：

```python
odrv0.axis1.requested_state = AXIS_STATE_ENCODER_OFFSET_CALIBRATION
```

5. 检查 `axis1.motor.is_calibrated`、`axis1.encoder.is_ready` 后保存校准：

```python
odrv0.axis1.motor.config.pre_calibrated = True
odrv0.axis1.encoder.config.pre_calibrated = True
odrv0.save_configuration()
```

首次不要设置 `pre_calibrated=True`，也不要恢复旧编码器偏置。

## 5. 闭环速度和位置测试

先使能速度闭环，并从低值逐级测试：

```python
odrv0.axis1.controller.config.control_mode = CONTROL_MODE_VELOCITY_CONTROL
odrv0.axis1.controller.config.input_mode = INPUT_MODE_PASSTHROUGH
odrv0.axis1.requested_state = AXIS_STATE_CLOSED_LOOP_CONTROL
odrv0.axis1.controller.input_vel = 0.5
```

依次测试 `0.5、1、2 turn/s`。随后速度阶梯为：

| rpm | `input_vel`（turn/s） |
|---:|---:|
| 30 | 0.5 |
| 60 | 1 |
| 120 | 2 |
| 300 | 5 |
| 600 | 10 |
| 1000 | 16.6667 |

每一级检查 `axis1.encoder.error`、`as5600_status`、`i2c_error_rate`、`sample_age`、`motor.current_control.Iq_setpoint`、电流波动、噪声和温升。1000 rpm、11 极对对应约 183 Hz 电频率；只有延迟补偿后换相稳定才通过，否则把最高速度限制在稳定值，或改用 SPI/ABI 编码器。

初始 `vel_limit=2 turn/s` 只覆盖到 120 rpm。继续测试 300/600/1000 rpm 前，应逐级把限速设置为略高于目标值，例如：

```python
odrv0.axis1.controller.config.vel_limit = 6.0    # 测试 300 rpm
odrv0.axis1.controller.config.vel_limit = 11.0   # 测试 600 rpm
odrv0.axis1.controller.config.vel_limit = 18.0   # 测试 1000 rpm
```

每次只修改并测试一级；发现错误、异常电流、噪声或温升立即回到 `AXIS_STATE_IDLE`。位置闭环应在速度测试稳定后再执行：

```python
odrv0.axis1.controller.config.control_mode = CONTROL_MODE_POSITION_CONTROL
odrv0.axis1.controller.input_pos = odrv0.axis1.encoder.pos_estimate + 0.1
```

## 6. 故障判据与排查

- 无 I²C 应答：确认 Pin 2/6 电源地、GPIO12=SCL、GPIO13=SDA、上拉电压和地址 `0x36`；旧 `enable_i2c_a` 必须为 `False`。
- `ABS_I2C_MAGNET_ERROR`：AS5600 `MD` 未置位，检查磁铁类型、轴向、气隙、偏心和模块供电。
- 一圈角度重复多次：磁环极对数不对，不能用 `cpr=4096` 继续校准。
- `CPR_POLEPAIRS_MISMATCH`：确认 `cpr=4096`、`pole_pairs=11`，且做的是机械一圈验证。
- 电阻校准失败/电压不足：确认 24 V 母线、限流电源、电机接线和 `resistance_calib_max_voltage=6`；不得在未限流时提高电流参数。
- 方向反：停止后检查电机相线和磁铁方向；必要时重新做方向搜索/编码器偏置校准，不要直接沿用旧 `direction`。
- 高速抖动或电流异常：检查 `sample_age`、I²C 错误率、相位延迟补偿和磁铁同心度；先降低速度，稳定性不足时改用 SPI/ABI。
- 总线卡死：固件会在不自动重新使能电机的前提下最多输出 9 个 SCL 恢复脉冲并重新初始化 I²C；错误保持锁存，必须人工清除故障并重新确认硬件。

停止电机命令：

```python
odrv0.axis1.requested_state = AXIS_STATE_IDLE
```

清错前先断开功率或确保电机安全；任何校准和速度阶梯测试均应准备硬件急停。

确认接线、磁铁和机械安全均已恢复后，错误不会自行清除或自动重新使能。由操作者明确执行：

```python
odrv0.clear_errors()
dump_errors(odrv0)  # 应确认无剩余错误
# 需要继续测试时才重新请求闭环：
odrv0.axis1.requested_state = AXIS_STATE_CLOSED_LOOP_CONTROL
```
