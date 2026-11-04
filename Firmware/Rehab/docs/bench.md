# V1.3 受限原生电机调试

本轮交付包含原有五个电机调试阶段及新增第6阶段：短时无编码器开环电气激励，使用同一份固件。T3 实物测试按用户要求跳过，保持未验收；已通过的通信、温度、HX711、编码器基本计数、启动 OV 和原始 ADC 结果保留。此固件不是完整康复设备的验收结论。

## 烧录与连接

在 Firmware 目录执行：

```text
make flash-bench
python -B Rehab/scripts/bench.py status
```

`make flash` 仍烧录锁定 full，不能替代 `flash-bench`。bench 启动不运动，地址为192.168.11.2/24、网关0、TCP39400；电脑沿用192.168.11.10。客户端只依赖 Python 标准库。

本版用于当前24V/1A台架、axis0。阶段2～5仍沿用软件配置4极对、高速轴AB编码器4000CPR；当前实际电机极对数不能由软件配置或无编码器开环试验确认。阶段1电机校准与阶段6不依赖编码器CPR、位置、速度或编码器校准通过标志；阶段6必须先有本次启动的motor_passed=1且motor_calibrated=1。全部阶段仍检查原生故障、GATE、STOP、母线、温度与ADC。不使用Z、不自动启动、不保存配置。NVM版本0x13B1隔离full0x1301；意外加载持久bench配置会禁止运动。普通Fibre/CAN/ASCII写入被拒绝，USB诊断读取和IDLE紧急停止保留。

初始状态应为enabled=1、active=0、armed=0、axis_state=1、gate_ready=1、physical_stop=0、encoder_cpr=4000、母线约24V、所有原生错误为0。motor_passed/encoder_passed初始为0正常。`hardware_locked`在没有有效台架许可时为1；生产硬件验收声明一直保持未通过。

## 分阶段命令

逐项查看结果再执行下一项。每个运动命令必须带`--execute`；缺少该参数时不连接设备。默认命令是只读status，不会自动串行执行所有阶段。

| 阶段 | 命令 | 固件限制与观察内容 |
|---|---|---|
| 电机校准 | `python -B Rehab/scripts/bench.py motor-cal --current 0.5 --voltage 0.5 --execute` | 首次明确使用0.5A/0.5V，最多8秒；检查motor_passed=1、motor_calibrated=1和本次R/L |
| 无编码器开环 | `python -B Rehab/scripts/bench.py open-loop --value 2 --current 0.5 --seconds 1 --execute` | 先通过电机校准；命令电角速度+2rad/s、0.5A、总期限1秒；不是已测转子速度 |
| 编码器校准 | `python -B Rehab/scripts/bench.py encoder-cal --execute` | 先通过电机校准；最多15秒，检查encoder_passed=1、encoder_ready=1 |
| 零转矩 | `python -B Rehab/scripts/bench.py zero --execute` | 最多5秒、命令电流上限1A，转矩输出强制0；观察armed期间Iq与实际转动 |
| 正向低速 | `python -B Rehab/scripts/bench.py velocity --value 0.1 --execute` | 最多10秒、命令电流上限3A |
| 反向低速 | `python -B Rehab/scripts/bench.py velocity --value -0.1 --execute` | 正向结果通过后执行 |
| 正向位置 | `python -B Rehab/scripts/bench.py position --value 0.5 --execute` | 相对起点0.5电机转，最多10秒 |
| 反向位置 | `python -B Rehab/scripts/bench.py position --value -0.5 --execute` | 相对起点-0.5电机转，最多10秒 |

电机校准`motor-cal --current`允许0.1～4A；编码器校准`encoder-cal --current`仍允许1～4A。两者0或省略仍表示旧版默认4A，不会因为一次失败自动提高电流。仅motor-cal支持可选`--voltage 0.1..3.0`，必须有限；该值同时用于原生电阻校准电压上限和电感校准电压脉冲。省略--voltage保持原8字节请求和既有行为，不自动切换到扩展请求或提高电压；本轮首条命令明确写0.5A/0.5V。旧板约0.0543Ω/143.4µH仅供比较，不复制R/L、编码器偏置、方向或预校准标志。

阶段6只用于短时观察电气激励，不是sensorless闭环，不测转子真实速度或极对数。`--value`为有符号电角速度rad/s，必须非零且绝对值≤20；`--current`范围0.1～1.5A，省略默认0.5A；`--seconds`范围0.2～5秒，省略默认1秒，必须精确到整数毫秒。全部参数须有限，0速度、NaN/inf、越界及小于1ms的时间细分均在连接前拒绝。可先运行不带--execute的同一命令核对参数，不会连接或运动。

阶段6复用原生LOCKIN_SPIN：电流爬升时间0.5秒，命令电角加速度10rad/s²；encoder.direction在该阶段临时设置+1，结束恢复原值，这不是编码器方向校准。请求总期限从Start被接受时起计时，包含进入状态等待与爬升，不保证电机实际使能满所填秒数，更不保证转子达到命令电角速度。客户端检查Start回显的电角速度和剩余期限，不匹配就停止；绝不自动重试Start。

阶段6屏幕中的position/velocity标为NOT_MEASURED；Iq/Id是按命令旋转坐标系投影的电流，不是真实转子转矩坐标下的转矩证明。原始日志仍保存原192字节状态和原始帧，并标记encoder_measured=false及commanded_electrical_frame，供排查用。旧固件若拒绝12/16字节请求，客户端记录错误、尽力Stop，不降级到其他参数或新序号重试。

速度value最高绝对值0.3电机转/秒，斜坡0.3转/秒平方。先验证±0.1再使用±0.3。位置value最高绝对值0.5电机转，目标在闭环建立后才发布；结束误差须≤0.02转且速度≤0.05转/秒，否则失能并记录Position故障。不是减速器输出轴转数。

每条成功命令的“Firmware phase COMPLETE”只表示固件阶段完成，不替代实际电流、方向、速度跟随和位置表现验收。停机后的保留Iq和待机合成相电流不能当作实时电机电流。

## 停止、清错、日志

- 运行命令窗口按Ctrl+C，客户端尽力发送Stop；即使传输失败，固件500ms心跳期限独立撤销许可。
- USB odrivetool可执行`dev0.axis0.requested_state = 1`作为锁存软件停止，不等于物理急停验收。
- `python -B Rehab/scripts/bench.py stop`在能建立TCP连接时停止。板端只有一个TCP连接，不能把第二个TCP连接当作运行中必达的急停通道。
- 错误先记录、排除原因，再显式执行`python -B Rehab/scripts/bench.py clear`。不自动清错，不重复清错掩盖问题；清错不恢复运动。
- 客户端每100ms喂心跳和查询、每秒打印；原始帧及状态日志保存在`build/rehab/bench_runs/*.jsonl`。状态直接包含原生编码器、电流、HX711序号及SPI计数，不需要虚构力标定或康复机械配置。

## 固定边界

所有阶段原生电流范数跳闸包络仍固定6A：idle/cal为current_lim5+margin1，zero为1+5，velocity/position为3+3；阶段6也保留6A跳闸。阶段6的1.5A只是命令上限，不是物理电流1.5A硬跳闸；同样，1A/3A命令限流不等于独立过流跳闸值。该包络避免把已观测到的约3A待机合成零偏当作真实相电流，不声明电流精度已验收。

起动母线22～26V、运行20～28V；两路名义温度须有限且-10≤T<50°C，原生45°C起降额。ADC须完成、驱动就绪、转换有效且年龄≤10ms。编码器相关阶段保留速度及行程保护：编码器校准速度保护1转/秒，闭环0.6转/秒；零转矩/速度/位置相对行程界限0.25/3.5/0.75转。阶段1和阶段6不把未测量的编码器数值当作速度/行程保护证据；阶段6依靠有界电气命令、总期限、心跳以及原有故障保护退出。进入目标原生状态不得超过500ms。心跳不能延长阶段总期限，重复帧不能喂狗，断连及新Hello替换原会话会撤销运动许可。

实物电阻记录为0.47Ω/50W，软件制动保持关闭；软件关闭不关闭STOP低电平下的硬件555支路。STOP、电流极性/精度、真实IRQ时间、制动回生及热能力仍未验收。本轮不进行力闭环、训练或人体测试。

state：0Idle、1Pending、2Running、3Complete、4Stopped、5Fault。

reason：0None、1Heartbeat、2Deadline、3Disconnected、4Stop、5Native、6Bus、7Temperature、8Adc、9Speed、10Travel、11State、12Prerequisite、13Position、14Numeric。

固定范围内调整校准电流、速度和位置不需要换固件。如果实板暴露软件缺陷或要求改变范围，须先征求用户意见，再修改代码。

## 请求兼容性

仍使用BenchStart命令22、现有Hello会话/序号/CRC和原192字节v1状态回复；原63字二进制Diagnostics不变。阶段1～5原8字节`<If>`请求保留。仅阶段1可追加校准电压成为12字节`<Iff>`。阶段6必须是16字节`<IffI>`，依次为stage=6、电角速度、命令电流、总期限毫秒；不能用旧8字节请求启动阶段6。
