# V1.3 原生优先、显式训练接管

起点：67a8be7c（FOCV2.2）。本次恢复复用现有 Engine，未改原生 FOC、电机校准、编码器、控制器算法和保护阈值；未连接硬件、烧录或运行电机。

## 当前行为

默认 `full`（stage 6），开机原生控制、轴待机、owner=0。网口保持 `192.168.11.2:39400`。`peripherals`（stage 2）可显式构建，仍只提供原生控制和基础外设。

未接管时，训练配置、HX711 和训练心跳均不限制原生三环；训练钩子不写控制器、不改变模式、不喂原生看门狗。原生 STOP、DRV、母线、电流及制动保护始终保留。默认过压仍可能为 0V，必须由用户按实际供电明确配置；本次未回填 24V 参数，也未自动保存配置。

Acquire 只在真正待机、未使能、无排队原生任务、无已准入配置写入时成功。首版拒绝 step/dir、无感模式、非增量编码器、其他负载编码器轴及活动抗齿槽控制。接管后原生诊断读取可用，原生参数写入和其他运动入口被拒绝；原生显式 IDLE 仍可紧急停止。

Enable 仅进入 Ready。Start/Resume 才能请求原生转矩闭环，要求原生电机已校准、编码器就绪、STOP/DRV/原生故障正常、真实力样本和完整训练配置有效。无台架许可或“三环通过”标志，不虚构力标定或机械参数。

首次 Start 前在任务态设置转矩/直通模式；ISR 不反复改模式。Finish 将转矩降至零并请求原生待机；确认真实待机/未使能后 Release 才恢复接管时的 control_mode/input_mode。退出清除转矩、速度和控制器积分，位置目标对齐有效当前位置；反馈无效时清除旧目标、保持待机，不发闭环请求。

训练故障、控制会话断连、心跳超时会停机并保留故障和控制权；观察会话断开无此副作用。相同会话 ClearFaults 后仍持有控制权，必须显式 Release 或重新 Enable/Start。原有断连恢复协议允许同一传输上的新会话显式 ClearFaults，在真实待机后清除并释放旧故障会话。物理 STOP 未恢复、DRV 未就绪或清错期间再次 STOP 都拒绝此恢复。任何恢复都不自动运动。

## 烧录与第一步（用户执行）

Firmware 目录的命令行：

```text
make flash
```

默认烧录 full。回退版本明确使用 `python -B Rehab/scripts/project.py flash --profile peripherals`。本次实现过程未执行上述命令。

重新连接 odrivetool，在 `In [...]` 先只读回：

```python
a = dev0.axis0
r = dev0.rehab
print("board/stage/owner:", hex(r.board_revision), r.stage, r.owner)
print("state/armed:", a.current_state, a.motor.is_armed)
print("STOP/gate:", r.physical_stop, r.gate_ready)
print("VBUS/UV/OV:", dev0.vbus_voltage,
      dev0.config.dc_bus_undervoltage_trip_level,
      dev0.config.dc_bus_overvoltage_trip_level)
print("calibrated/encoder ready:", a.motor.is_calibrated, a.encoder.is_ready)
dump_errors(dev0)
assert r.board_revision == 0x1301 and r.stage == 6 and r.owner == 0
assert a.current_state == 1 and not a.motor.is_armed
```

随后按实际电机参数使用原生接口测试转矩/电流、速度、位置。先确认供电保护阈值、电机类型、极对数、编码器 CPR、转矩常数和校准结果；不套用旧电机参数。原生测试完全不需要连接训练客户端。原生闭环请求仍是 `a.requested_state = 8`，停止仍是 `a.requested_state = 1`；本说明不把示例状态号当作无需核对参数即可启动的测试脚本。

## 原生测试通过后再连接训练客户端

在同一个 odrivetool `In [...]`：

```python
import runpy
api = runpy.run_path(r"D:\00-ODrive-text\ODrive-fw-0.5.6\Firmware\Rehab\scripts\training_client.py")
c = api["TrainingClient"]().connect()
print(c.status())
print(c.diagnostics())
```

这里只 Hello/查询，不接管、不启动心跳、不运动。确认原生已待机且未使能之后：

```python
assert a.current_state == 1 and not a.motor.is_armed
c.acquire()
print(c.status())
```

Acquire 成功后客户端才启动 50ms 心跳。请求串行执行，验证 CRC、序号、会话和响应长度；失败不重发、不自动清错、不重启训练。检查 `c.check_error()` 可在主线程报告后台错误。默认单次请求总超时 200ms，可明确设置但不超过 2s；长延迟可能触发训练心跳保护，不能视为已保证实时性。

力传感器未到位时停在查询，不 Enable/Start。到位后在真实空载、稳定新样本条件下调用 `c.tare()`；放上已知载荷、等待新的稳定样本后调用 `c.calibrate(known_force_n)`，单位 N。两个操作分开执行，不能在同一批未改变的样本上完成。校准系数可用 `c.get_config()` 读取。

配置先 `c.set_config({...})`（每次最多 32 字段），再 `c.commit_config()`；只写 RAM，不保存 Flash。`api["CONFIG_FIELDS"]` 是与固件一致的字段表。必须填写实测卷筒半径、传动比、编码器比、方向/零点、效率、长度/速度/力/转矩限制、目标力和 PI 参数；使用真实去皮/标定系数。先去皮/标定再将真实机械和限制参数分批暂存，最后提交；提交后等待新的有效力样本。不要用示例标定系数或零参数骗过配置检查。

确认 Status 的 config_valid、force_valid、faults、采样年龄及机械量符合实际，再分开执行：

```python
c.enable()                 # 只 Ready，不运动
print(c.status())
# 用户明确准备开始当前模式后，才执行下一行：
c.start("ISOTONIC")
```

模式名：ISOTONIC、CONCENTRIC、ECCENTRIC、ISOKINETIC、ELASTICITY、WATER_RESISTANCE、FITNESS。方向、去皮、已知力标定、限制和实机保护尚须独立验收，主机测试不代表人体训练可用。

正常结束：

```python
c.finish()
# 等待实际状态，不自动重试运动命令。
print(c.status(), a.current_state, a.motor.is_armed)
```

确认 Status.state=0、a.current_state=1、未使能后：

```python
assert a.current_state == 1 and not a.motor.is_armed
c.release()
c.close()
print("native owner:", r.owner)
```

`c.stop()` 是立即锁存停止；`c.close()` 不隐式 Finish/Release，训练拥有者断开会故障停机并保留旧控制权。异常先保留错误查询，排除原因后显式 `c.clear_faults()`；若客户端因错误已关闭，应重新 connect 后查询，再显式恢复。不要盲目循环清错或 Start。

## 协议与验证边界

命令 1～21 恢复原能力；22～26 保持 Unsupported。Hello 接口标识 0x1201 不表示 PCB 版本；实际 PCB 由 Fibre board_revision=0x1301 确认。Status=64 字节，Diagnostics=4 字节结果码+63个uint32，旧采样/ISR统计槽仍为保留值。已恢复训练状态、控制权、力值、长度/速度和统计字段。配置版本保持 0x1301。

验证入口：

```text
python -B Rehab/scripts/test.py --quick --cxx D:/mingw64/bin/g++.exe
python -B Rehab/scripts/build.py full --board v13
python -B Rehab/scripts/resources.py full --board v13
python -B Rehab/scripts/project.py check
```

测试使用真实 Engine、Service、V1.3 运行层代码和明确的硬件/RTOS替身，客户端测试只连接本机假服务器。未测真实 IRQ 时序、任务栈峰值、机械行为或力控性能；本轮不把旧电流跳变问题当作已经验证解决。
