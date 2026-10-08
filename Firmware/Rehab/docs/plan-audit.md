# V1.2 原计划逐项核对

核对日期：2026-10-04。范围是用户批准的五部分固件计划，以及后续要求的 VS Code 工程修复。
在逐项审计后，本轮按用户要求补齐原生入口运行测试、完整ISR诊断和结构化命令队列，并重新执行软件验证。

**软件实现和软件验证与实机验收分别记录。** 下文A/B/C记录补齐内容，最终执行数字以
[验证报告](verification.md)、[软件结果](software-results.json)和[资源表](resources.md)为准。实机仍待验证。

## 一、基线与单轴板

| 要求 | 核对证据 | 结论 |
|---|---|---|
| 上游版本、原有修改、快照、SHA256 | `analysis/rehab_v12_baseline`；`check_core.py`校验570个原路径、59个审查文件 | 符合；新增审查项仅STOP完整IRQ计时接入 |
| 原生FOC、电流环、SVPWM、速度/位置算法不变 | 保护清单中的原算法文件、Board/v3哈希未变化；修改集中在固定接入点 | 符合 |
| V1.2仅M0、原v3保留、单轴接口 | `Board/rehab_v12/Inc/board.h`的AXIS_COUNT=1；`Rehab/interface.py`；9个实际ELF与endpoint检查 | 符合软件要求 |
| TIM1/TIM8/TIM13调度保留，TIM8无外部PWM | 板级TIM8 CCER/MOE清零，无第二轴对象；保留ADC齐备、漏更新和deadline检查 | 已实现；物理时序待验收 |
| ADC白名单，释放PC2/PA4，USART2 | ADC2/3通道10/11；后台ADC仅5/6/15；UART_A=&huart2 | 符合软件要求 |
| 500µΩ、19倍分压、8MHz HSE、DRV原生增益 | `board.h`、HAL时钟配置、原DRV8301驱动 | 已按证据配置；采样极性待实测 |
| 独立配置版本，拒绝旧双轴配置 | V1.2 CRC版本种子0x1201，原板0x0001；实际ConfigManager两版各397项检查 | 符合；没有新增Flash分区 |
| 应用参数仅RAM、待机原子提交 | `Engine::configure`和`Service::execute`的状态/失能检查、整对象提交 | 符合 |

## 二、控制权、并发与保护

| 要求 | 核对证据 | 结论 |
|---|---|---|
| Native/Training交接、静止失能、无未完成请求 | `take_control`、`quiescent_axis`、native_writers临界区；运行期交接用例 | 已实现并有模型证据 |
| 暂停保留控制权，释放前确认失能 | Engine暂停/结束状态机；释放时清input_torque/input_vel/torque_setpoint | 已实现并有运行回归 |
| Fibre/函数/ASCII/CAN/PWM/analog/step-dir/轴入口门禁 | 真实生产入口及生成stub运行测试；调用链与提取SHA清单 | 补齐A，边界详见入口测试说明 |
| 不改变原生报文、ACK不作为业务成功 | 原生ACK保留；拒绝需读回诊断；扩展协议有Result | 符合，文档明确限制 |
| 应用单写者、固定容量输入、1kHz服务 | 每端口512B RX、1024B TX；应用任务内完整命令8槽FIFO，每轮最多4条 | 补齐C，含STOP旁路和旧字节边界 |
| 带序号/代次/期限目标、快环检查和短临界区反馈 | Target包含sequence/epoch/created_us；平台检查epoch、期限、限值，快照短临界区发布 | 已实现；sequence用于发布标记，当前内部没有重排目标队列 |
| STOP/失联/陈旧/越界/驱动故障本地失能 | fast_guard、control_hook、trip、Motor::arm/PWM二次准入；Engine各故障位 | 已实现并有软件故障注入 |
| 暂停与正常结束受限卸力、超时立即失能 | Pause/Finish→Stopping→Paused/Standby；Stop为独立立即锁存 | 符合 |
| CLEAR、ENABLE、恢复独立；重连不恢复旧目标 | 清错代次竞争回归、断连租约、HELLO/Acquire/Enable/Start分离 | 符合 |
| 模块关闭无专属任务/外设，安全输入保留，DMA在SRAM | 功能宏、ELF符号检查、DMA对象地址检查；网络关闭无network任务，安全任务保留 | 符合软件要求 |
| 记录ISR耗时、栈、队列峰值、内存 | 五类完整IRQ函数体峰值/计数、hook/服务峰值、栈水位、命令/RX/SPI峰值、ELF资源表 | 补齐B，实时水位和期限待上板测量 |

## 三、外设、算法与协议

| 要求 | 核对证据 | 结论 |
|---|---|---|
| SPI3唯一仲裁、DRV优先、容量/长度/等待上限 | 8等待+1活动、原生优先级0/网络3、最大260B、入队期限；静态DMA bounce buffer | 已实现 |
| SPI失败/超时取消/迟到回调/恢复 | 真实驱动模型；取消后无DMA引用调用者栈；无法停止时quarantine；ISR重配延后任务 | 730项驱动检查的一部分；硅片故障时序待验收 |
| HX711 10/80SPS、真新样本、符号/滤波/标定/陈旧 | 驱动最小转换间隔、25脉冲后DOUT高确认；核心按序号/时间处理；高脉冲独立短临界区 | 已实现并有模型测试；当前板级默认80SPS，物理脉宽待测 |
| CH394Q独立寄存器、复位/链路/socket/恢复 | `ch394.cpp`的16位地址+控制字；静态IPv4、Socket0 TCP；低优先级分片任务 | 已实现并有模型测试；实际芯片/网络待验收 |
| SI单位和机械换算 | Config及Engine换算；编码器安装位置用encoder_turns_per_motor_turn表达 | 符合，真实参数未代填 |
| 等张前馈、低带宽反馈、新力样本积分/抗饱和 | desired_torque及update_force；转矩/斜率限幅用例 | 符合软件要求 |
| 向心/离心、方向/迟滞/位移/持续时间 | update_phase、独立concentric/eccentric目标；零速不直接翻转张力方向 | 符合软件要求 |
| 等速上限阻力、弹性相对位移、水阻速度平方 | force_goal及统一保护/限幅 | 符合；等速不是强制速度轨迹 |
| 主动回程独立目标、速度/行程/力/超时，默认关闭 | ReturnEnabled默认0；开启时独立离心目标及回程限制 | 已实现并有边界测试 |
| 次数、力量/速度、功率、分方向能量、RFD | Statistics、真实样本窗口；24h参考场景 | 符合软件要求；功率为绳端机械功率 |
| 体能准备/采集/结束/结果 | Ready→Fitness Running→Stopping→结果有效位 | 符合简化流程；未冒充原机完整程序 |
| 摩擦/惯性独立限幅、默认关、辨识参数约束 | flags、CompensationIdentified、数值校验、独立compensation_limit_nm | 符合软件要求；真实辨识待做 |
| Release明确不支持 | Mode::Release/Start拒绝测试 | 符合已确认边界 |
| 配置/查询/去皮/标定/控制/心跳及明确结果 | Service命令1..21、Result | 符合 |
| USB Fibre保留，CDC/UART/TCP单流单协议，无打印混入 | Stream和原生通信接入；Rehab活动期间阻止stdout | 已实现；Stream生命周期300项检查 |
| 版本帧、256B、会话/序号/CRC32、分包/重同步 | Frame/Parser；协议11场景31959断言 | 符合软件要求 |
| 副作用去重、仅owner有效新心跳续期 | Replay保留完整请求比较；重复HB不续期、旧会话拒绝 | 符合 |
| 只读能力/状态/故障/样本年龄/拒绝/资源 | Fibre RehabStatus及63-word Diagnostics，含队列和五类IRQ | 已实现；原有0..47索引保留 |

## 四、交付与验证

九个ARM构建的源码摘要和镜像SHA256记录在 `build/rehab/*/build.json`；资源脚本拒绝旧源码对应的镜像。
最终汇总见 `Rehab/DELIVERY.json` 与 `build/rehab/tests/report.json`，8个V1.2 profile均为真实使能关闭。
主机矩阵包含8种核心开关、16种业务开关、驱动、运行期、协议、流、NVM、原生入口和CLI仿真。
24h长测直接运行生产Engine/Service，没有另写Python训练算法；原生step_cb超长用例仍明确排除，
由下述有界真实函数测试补充。

构建、测试、核心校验、资源脚本及协议、配置、回退、实机清单均存在。
VS Code编译器路径、真实头文件、独立生成接口和默认串行构建入口已修复，main.cpp ARM语法检查通过；
用户随后回复“OK”。没有直接读取当前问题面板数量，不伪造“观察到0条”的证据。

## 五、实机默认锁及待验收

commissioning.hpp仍保留：制动HS/回生、电流极性、电机/编码器、时序确认false，STOP电平-1，
NTC B值及温度/母线阈值未填写；编译开关也为0。模拟配置没有解除这两层真实使能限制。
U12 HS=`G_HS`与半桥中点=`$5N34`分网的已有证据、修正建议及待验收列表均保留。
本次没有上位机、烧录、电机驱动或电路修改。实机项目没有任何“实机通过”记录，符合本轮工作边界。

## 本轮补齐项及验证边界

**A．原生入口生产代码运行回归。**
新增 `Tests/rehab/ingress_tests.cpp`，运行52个从源码完整提取、未改写的入口函数，包含ASCII、CAN、
PWM、analog及Axis step-dir；真实生成接口、函数stub、Property与整个runtime直接编译。
提取器记录完整函数SHA、原行号、生成器/模板来源及14项调用链检查，测试每次重新生成。
覆盖Native、实际Engine Running/Paused、显式停止、清错与释放后不自动使能、step重启；
step使用正负边界和固定种子替代约86亿次原测试，不声称该原测试已经执行。
HAL/RTOS、硬件承载对象和小型属性目录为替身；完整endpoint树、物理总线和轴RTOS循环不在主机模拟范围。
详细边界见 `Tests/rehab/ingress/README.md`。

**B．完整ISR函数体诊断。**
ControlLoop、TIM8更新、SPI DMA TX/RX和STOP所在EXTI9_5的首尾计时已接入，C++ RAII覆盖提前返回。
每通道输出峰值周期和次数，IRQ固定次数原子读写、不等待锁；主机测试覆盖早退、回绕、通道隔离、
抢占计时和PRIMASK恢复。计时包含抢占，不含硬件异常进退栈及最后统计写入；实际WCET仍待实测。

**C．结构化命令队列及事件边界。**
新增固定8槽FIFO、每轮最多4条，满返回QueueFull；每条带端口、会话帧、控制权/故障/传输代次及时间。
STOP经会话/重放检查旁路普通队列；过期或旧代次拒绝。额外处理原始字节缓存、部分帧和接收块复制途中
发生STOP的边界，避免旧CLEAR/ENABLE/START被重新标记后执行。持续故障镜像不制造伪新代次；
断连发布与TX清空原子化，旧命令和旧响应不跨连接。队列回绕、满载、重试、STOP旁路、旧CLEAR和
跨周期清错均有真实runtime测试。队列仅在协议开启时分配，ARM符号检查确认关闭时消失。

软件验证只覆盖所述主机模型与ARM构建，**没有条目标记为“实机通过”**。
STOP接法、NTC、电机/编码器/机械参数、采样极性、HX时序、中断期限、温升和制动回生仍按实机清单验收。
