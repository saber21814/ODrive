# V1 二进制协议与 RAM 配置

实现源：`Firmware/Rehab/protocol/{frame,service,config_fields}.{hpp,cpp}`。
USB Vendor Fibre 保留；启用协议的 V1.2 profile 将 USB CDC 设为专用 Rehab 流。
USART2 的逻辑 UART_A 可在原生待机配置中选择 `STREAM_PROTOCOL_TYPE_REHAB`；
串口波特率仍使用原生 `uart_a_baudrate`。同一流只选一种协议，不能叠加 ASCII/stdout。
协议选择在连接/启动时应用；修改后应重新连接USB或重启串口服务。运行中的Rehab流会阻止stdout发送，即使原生客户端改动了协议配置字段。
CH394Q TCP 使用同一帧格式，单 Socket0 服务端；IP 未设置时不启动监听。
USB/UART/TCP 分别为端口0/1/2，各有独立会话、组帧器和去重缓存。

## 帧格式

除 IP 的逐字节地址外，所有多字节整数、float32 位模式均为小端；禁止直接发送 C++ 结构体内存。

| 偏移 | 字节数 | 含义 |
|---:|---:|---|
| 0 | 2 | ASCII `RH`，即 `52 48` |
| 2 | 1 | 版本1 |
| 3 | 1 | 请求0；响应1；其余拒绝 |
| 4 | 2 | payload长度，0..256 |
| 6 | 2 | command |
| 8 | 4 | session |
| 12 | 4 | sequence |
| 16 | length | payload |
| 16+length | 4 | CRC32，覆盖帧头及payload |

总长20..276字节。CRC为反射IEEE CRC32：多项式 `0xEDB88320`、初值 `0xFFFFFFFF`、最终按位取反；
与 `zlib.crc32` 一致。解析支持任意分包/粘包、逐字节坏帧重同步。半帧超过250 ms没有新字节则丢弃，
空轮询不会续期。坏帧不执行命令、不喂心跳；本地故障期限继续生效。

每个响应 payload 先放一个 **uint32 结果码**，成功时才按下表解释后续字段。
响应重复 command/sequence；HELLO成功时响应头携带新 session。

| 码 | 名称 | 意义 |
|---:|---|---|
| 0 | Ok | 已接受/完成本命令；网络初始化和卸力操作仍须查询状态 |
| 1 | InvalidState | 状态不允许 |
| 2 | InvalidConfig | 长度、字段、数值或参数组合无效 |
| 3 | Busy | 轴未静止失能、交接中或被其他控制者占用 |
| 4 | NotOwner | 不是当前控制会话/端口 |
| 5 | Faulted | 有故障锁存 |
| 6 | Unsupported | 模块/模式未编译或Release算法不支持 |
| 7 | Stale | 标定等操作缺少新的有效样本 |
| 8 | BadSequence | 会话不匹配、过期重放、同序号不同内容 |
| 9 | QueueFull | 保留的业务准入码；字节环溢出由诊断和本地失能上报 |
| 10 | Inhibited | 硬件未验收、输入无效或安全条件未满足 |

## 命令

下表“返回”均不包含前置结果码。未列 payload 的请求长度必须为0。

| ID | 命令 | 请求payload | 成功返回/行为 |
|---:|---|---|---|
| 1 | Hello | 无，session必须0 | uint32: 版本1、最大payload256、板接口标识0x1201；头部新session |
| 2 | Status | 无 | 见状态格式 |
| 3 | SetConfig | 1..32个8字节字段项 | 原子修改本连接的暂存配置；不立即应用 |
| 4 | GetConfig | first:uint16, count:uint16, selector:uint32 | count个8字节字段项；count=1..31；selector0运行/1本连接暂存 |
| 5 | CommitConfig | 无 | 验证整个暂存对象并一次提交；待机且实际失能；失败保留旧配置 |
| 6 | Acquire | 无 | 申请训练控制权；轴稳定IDLE、失能、无原生命令/校准/step-dir等 |
| 7 | Release | 无 | 仅待机/故障、实际失能时释放；不代表Release训练模式 |
| 8 | Enable | 无 | 进入Ready；自身不出力；当前实机镜像返回Inhibited |
| 9 | Start | mode:uint32 | Ready后开始；Release模式返回Unsupported |
| 10 | Pause | 无 | 受限卸力，实际失能后Paused，仍保留控制权 |
| 11 | Resume | 无 | Paused显式恢复，重新检查输入、心跳、故障和硬件准入 |
| 12 | Stop | 无 | 紧急锁存FaultStop，立即调用原生disarm；已协商的观察端口也可停 |
| 13 | ClearFaults | 无 | 清故障；不使能、不恢复；要求实际IDLE/失能和停止输入已释放 |
| 14 | Heartbeat | 无 | 只有当前控制会话的新的有效序号续期 |
| 15 | Tare | 无 | 8个稳定原始新样本的去皮；无需先有完整机械配置 |
| 16 | Calibrate | known_force_n:float32 | 去皮后取得8个新的稳定加载样本，双点标定 |
| 17 | Statistics | 无 | 见统计格式；未编译统计返回Unsupported |
| 18 | Diagnostics | 无 | 48个uint32，见诊断格式 |
| 19 | SetNetwork | IP[4],gateway[4],netmask[4],port:uint16，共14字节 | 待机无控制者时，异步接收静态IPv4配置；需查询网络状态 |
| 20 | GetLog | 无 | uint32: found、HAL时间ms、code、value；取出最早一条；64条满时丢最早 |
| 21 | Finish | 无 | 当前控制者正常结束，按变化率卸力，实际失能后Standby，随后Release |

准备体能测试使用 Enable/Ready → Start(Fitness)/采集 → 自动Finish/结束 → Statistics/结果流程。
没有取得原机完整程序，不把该流程标为原机算法复现。

HELLO从MCU硬件RNG取非零会话；未就绪/报错/碰撞返回Inhibited，可重新请求。
控制会话存在时，同端口不能直接重新HELLO覆盖它。每连接缓存最近4条完整请求/响应；
同session/sequence且内容完全相同直接返回旧响应，**不重复执行副作用，也不续期心跳**。
旧于缓存或同序号不同内容拒绝，即使CRC碰撞也不能冒充同一请求。
sequence须单调增加，不允许在同会话内回绕。重连必须重新HELLO；session不是网络鉴权密钥。

控制端口断连会锁存通信故障并保留故障控制权。原控制端口重连HELLO后，显式CLEAR在IDLE/失能时
清除并释放旧租约；随后重新ACQUIRE、ENABLE、START。其他端口不能抢占这个故障租约；
不会因重连或heartbeat自动运动。原生 clear_errors 在训练控制权下也被拒绝。

## 配置字段

SET/GET字段项：`id:uint16, type:uint16, bits:uint32`；type0=float32，type1=uint32。
所有浮点必须有限，NaN/Inf拒绝。未知字段、类型不符、范围或组合不合法时拒绝整个提交。
配置仅在RAM；暂存按连接隔离。去皮/标定只同步该连接的力比例/零点，不覆盖已暂存的机械字段。

| ID | 名称 | 类型/单位 | 约束/定义 |
|---:|---|---|---|
| 0 | radius_m | float m | 有效卷筒半径>0 |
| 1 | motor_turns_per_drum_turn | float 比值 | 电机转数/卷筒转数>0 |
| 2 | encoder_turns_per_motor_turn | float 比值 | 编码器所在轴转数/电机转数>0；安装位置由该比值表达 |
| 3 | direction | float | 仅+1/-1；将编码器正方向映射到放绳方向 |
| 4 | zero_turns | float 编码器turns | 绳长零点 |
| 5 | efficiency | float | 0<效率≤1 |
| 6 | force_scale_n_per_count | float N/count | 非零，允许负标定方向 |
| 7 | force_offset_counts | float count | 去皮原始计数 |
| 8 | force_filter_hz | float Hz | 0<截止≤40，随真实样本dt更新 |
| 9 | min_length_m | float m | 下行程边界 |
| 10 | max_length_m | float m | 必须>下边界 |
| 11 | max_speed_m_s | float m/s | 绝对速度硬限值>0 |
| 12 | max_force_n | float N | 原始/滤波力硬限值>0 |
| 13 | max_torque_nm | float N·m | 电机轴转矩限幅>0 |
| 14 | torque_slew_nm_s | float N·m/s | 正常加载/卸力变化率>0，严重故障绕过斜率直接失能 |
| 15 | force_kp | float N·m/N | ≥0，低带宽力比例反馈 |
| 16 | force_ki | float N·m/(N·s) | ≥0，仅新力样本积分 |
| 17 | integrator_limit_nm | float N·m | 0..max_torque，抗饱和积分限幅 |
| 18 | concentric_force_n | float N | 0..max_force，放绳阶段/等张目标 |
| 19 | eccentric_force_n | float N | 0..max_force，独立收绳阶段目标 |
| 20 | speed_limit_m_s | float m/s | 0<速度上限≤max_speed |
| 21 | elastic_n_m | float N/m | ≥0，相对本次起点的弹性系数 |
| 22 | water_n_s2_m2 | float N·s²/m² | ≥0，水阻力=系数×速度² |
| 23 | phase_speed_hysteresis_m_s | float m/s | >0且<max_speed |
| 24 | phase_min_distance_m | float m | >0且<总行程；相位/次数最小位移 |
| 25 | phase_dwell_ms | uint32 ms | 1..2000000，相位确认持续时间 |
| 26 | heartbeat_timeout_ms | uint32 ms | 1..2000000，默认250 |
| 27 | sensor_timeout_ms | uint32 ms | 1..2000000，默认250；10SPS需考虑100ms周期 |
| 28 | target_timeout_ms | uint32 ms | 1..2000000且≤心跳/传感期限，默认20 |
| 29 | stop_timeout_ms | uint32 ms | 1..2000000，受限卸力超时转立即失能，默认2000 |
| 30 | return_speed_m_s | float m/s | 0..max_speed；主动回程开启时必须>0 |
| 31 | return_timeout_ms | uint32 ms | 1..2000000，默认5000 |
| 32 | fitness_duration_ms | uint32 ms | 1..2000000，默认10000 |
| 33 | rfd_window_ms | uint32 ms | 25..1000，默认200；须有足够真实样本覆盖 |
| 34 | friction_nm | float N·m | ≥0，已辨识库仑摩擦 |
| 35 | viscous_nm_s | float N·m·s/rad | ≥0，已辨识粘性系数 |
| 36 | inertia_kg_m2 | float kg·m² | ≥0，折算到电机轴的已辨识惯量 |
| 37 | compensation_limit_nm | float N·m | 0..max_torque；启用补偿时>0 |
| 38 | flags | uint32 bitmask | bit0主动回程；bit1摩擦；bit2惯性；bit3辨识有效；未知位拒绝 |

启用摩擦/惯性必须同时声明辨识有效并提供有效限幅；未编译补偿的profile拒绝这些flags。
所有真实机械、电机、标定值都必须来自实机确认；默认缺失参数不会形成有效运动配置。
过大的参数导致单位换算溢出也会拒绝。

单位换算：

```text
length_m = (encoder_turns - zero_turns) * direction * 2π * radius_m / gear / encoder_ratio
speed_m_s = encoder_turns_per_s * direction * 2π * radius_m / gear / encoder_ratio
tension_feedforward_nm = force_n * radius_m / gear / efficiency
command_torque_nm = -direction * bounded_tension_nm
```

ODrive原生编码器`cpr`仍须正确设置；应用接收原生turns，不把PPR直接当turns/counts。
力PI含抗饱和、转矩限幅和变化率限制。零速附近保持机械张力方向，不按瞬时速度翻转转矩。
向心/离心由方向映射、速度迟滞、最小位移和持续时间识别；生理学相位需要按机构映射确认。
等速是“速度上限型阻力”，不是强制速度轨迹。

模式枚举：0等张、1向心、2离心、3等速限速阻力、4弹性、5水阻、6体能测试、7Release不支持。
主动回程默认关闭：负放绳速度时命令转矩立即归零，避免残留卸力斜率主动拉回。
打开后使用独立离心目标，接近速度/下行程限制时降力；不能通过增大内拉转矩来制动内收运动。
严重故障、硬行程/力/速度超限始终立即失能。

## 状态、统计与故障

STATUS成功payload总64字节（含结果码），之后顺序为：

```text
uint32 state, mode, faults, owner_session, epoch, config_valid, force_valid, sample_age_us
float32 length_m, speed_m_s, force_n, target_torque_nm
uint32 target_enable, rfd_valid, hardware_motion_allowed
```

状态：0 Standby、1 Ready、2 Running、3 Stopping、4 Paused、5 Fault。
`hardware_motion_allowed`同时考虑硬件声明、STOP及锁存，不是“已经arm”；实际arm由Diagnostics给出。
STATISTICS成功payload总56字节：

```text
uint32 repetitions, force_samples
float32 elapsed_s, peak_force_n, mean_force_n, peak_speed_m_s, mean_speed_m_s,
        power_w, positive_energy_j, negative_energy_j, rfd_n_s, peak_rfd_n_s
uint32 fitness_result_valid
```

平均力按有效新样本计，平均速率按时间积分；功率=力×有向速度，分方向能量均以非负幅值报告。
RFD对真实滤波力样本的时间窗口插值差分；数据不足或暂停/故障时无效，不用1kHz任务频率冒充传感频率。
一次完整次数需要确认放绳、收绳及下一次反向，最后的未完成往复不计。暂停不清已有统计，新的START清统计。

故障位：bit0 STOP、1原生驱动、2传感器、3心跳/断连、4绳长、5速度、6力量、7数值、
8卸力超时、9回程超时、10目标/反馈过期、11平台故障。
故障可组合；清故障后必须重新ENABLE和START，不允许直接RESUME。

DIAGNOSTICS成功payload为结果码+48个uint32（196字节）。同名只读属性位于Fibre的 `odrv.rehab`：

| word索引 | 内容（按顺序） |
|---|---|
| 0..7 | state, mode, faults, owner（0原生/1训练）, epoch, config_valid, force_valid, sample_age_us |
| 8..13 | native_rejected, native_writes（当前进行数）, hardware_locked, stage, hx711_enabled, network_enabled |
| 14..20 | command_rejected（业务失败计数）, rx_overflow, tx_overflow, max_hook_cycles, max_service_cycles, safety_stack_free（字节）, network_stack_free |
| 21..27 | spi_timeouts, spi_errors, spi_rejected, spi_highwater, network_errors, hx711_timing_faults, log_dropped |
| 28..34 | repetitions, motor_armed, pending_rx_bytes, force_raw_bits（重解释int32）, force_sequence, force_raw_age_us, force_raw_valid |
| 35..41 | spi_quarantined, network_state, network_last_error, network_link, network_connected, network_chip_version, network_reconnects |
| 42..47 | rx_highwater_bytes（单端口峰值）, last_native_rejection（Result）, bad_crc, bad_header, partial_frame_expired, uptime_ms |

Fibre另提供float：length_m、speed_m_s、force_n、target_torque_nm、power_w、positive_energy_j、
negative_energy_j、rfd_n_s、vbus_v、fet_temperature_c。
`max_hook_cycles`只测应用控制hook，不能冒充整个原生控制ISR的WCET；栈水位每秒采样一次。
CH394 network_state对应驱动枚举0 Disabled、1 ResetLow、2 ResetWait、3 Identify、4 Configure、
5 OpenSocket、6 WaitCommand、7 WaitOpen、8 Monitor、9 Backoff；last_error：0无、1配置、2SPI、
3寄存器回读、4命令超时、5Socket超时、6IP冲突、7计数异常。
PHY就绪、TCP连接、HELLO会话和训练控制权是不同状态，不能互相代替。
