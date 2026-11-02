# V1.3 固件迁移与验收

适用在线工程“无轨迹电机控制板V1.3”，2026-10-29 审查。工程 UUID：
`4a15ace9143ca76b39ed4250ddfdb2dea82ecd3aa157ac7e91711f5a211e853a`；PCB UUID：`493f9c83e05d109e`。
关键器件原理图与 PCB 网表一致。PCB DRC 接口报“指定的主题消息在对应的画布内没有相关订阅”，布线规则验收仍未完成。

本次只修改和验证软件，没有烧录、驱动电机或修改电路。首轮采用 ABZ 编码器和禾川750W电机；额定功率不能代替极对数、电阻、电感、转矩常数、电流及机械参数。

## 构建及调试顺序

在仓库 `Firmware` 目录执行：

```powershell
python -B Rehab/scripts/check_core.py
python -B Rehab/scripts/test.py
python -B Rehab/scripts/build.py native --board v13
python -B Rehab/scripts/build.py hx711 --board v13
python -B Rehab/scripts/build.py network --board v13
python -B Rehab/scripts/build.py peripherals --board v13
python -B Rehab/scripts/build.py protection --board v13
python -B Rehab/scripts/build.py training --board v13
python -B Rehab/scripts/build.py statistics --board v13
python -B Rehab/scripts/build.py full --board v13
python -B Rehab/scripts/resources.py all --board v13
```

`build.py all --board v13` 包含原 v3.6-56V 回归和以上八个 V1.3 阶段；`verify.py --board v13` 同时执行核心保护、完整主机测试、全部 ARM 构建和资源检查。
`native` 保留原生功能及平台安全门禁，再逐步加入外设、保护、训练、统计和补偿。关闭网口时不初始化专用 SPI1 或启用其 DMA 中断。
所有阶段的 `REHAB_REAL_MOTOR=false`，真实准入声明继续锁定。编译通过不能代替实机阶段验收。

- V1.3 产物：`build/rehab/v13/<profile>/ODriveFirmware.{elf,hex,bin,map}`，同目录有 `build.json`、`build.log` 和接口快照。
- full 自动保存编辑器命令及生成头到 `build/rehab/v13/editor/`，不自动覆盖 VS Code 配置。
- V1.2 原命令及输出目录保留；不带 `--board` 时仍构建 V1.2。
- `baseline` 产物仅用于原板回归，不能装到 V1.3。
- 构建按字节恢复原 `tup.config`；中断可用 `build.py <profile> --recover-config --board v13` 恢复。一次只运行一个 ARM 构建进程。
- 当前电脑曾出现 `cc1plus.exe: out of memory`；保留失败日志后串行重试通过。这类编译器资源错误不作为源码通过或失败的功能结论。
- 主机日志及报告：`build/rehab/tests/`。当前提供代码测试、协议回归和动力学仿真，未新增上位机图形界面。

### Windows 终端构建修复（2026-11-02）

终端中的 `make -j8` 调用旧 Makefile 时，没有把并发数传给内部的 Tup。该电脑上 Tup 默认启动32个任务，出现 `.tup/tmp/backup-*`、`output-*` 访问冲突以及编译器头文件 `Invalid argument`。
已在此工作副本的 `Firmware/.tup/options` 中设置：

```ini
[updater]
num_jobs = 1
```

该设置只影响本工程的构建并发，不改固件源码、原 Makefile 或编译器头文件；依据 [Tup 官方选项说明](https://gittup.org/tup/manual.html)。`.tup` 不受 Git 跟踪，重新克隆或重建该目录时需重新设置；显式传给 Tup 的 `-j` 仍会覆盖该值。
原 `make -j8` 已在 Git Bash 中复编通过，完整日志为 `build/rehab/build_fix_20261102/make-j8.log`。`Environment variable changed: HOME/PATH` 是环境变化提示，不是这次失败原因。
V1.3 full也已复编通过，资源、产物哈希及源码保护检查通过；本次记录为 `build/rehab/build_fix_20261102/verification.json`。
原 Makefile 仍读取 `tup.config` 中的 `v3.6-56V`。V1.3 请使用上面的 `python -B Rehab/scripts/build.py full --board v13`，不要把原板产物用于V1.3。

## 板级映射与隔离

| 功能 | V1.3 |
|---|---|
| 母线采样 | PA1 / ADC通道1，普通及注入采样；18k/1k，倍率19 |
| 制动温度 | PB0 / ADC通道8 |
| M0温度 | PC5 / ADC通道15 |
| STOP | PC4 / EXTI4，低有效、外部10k下拉、无内部上拉 |
| CH394Q | SPI1 PA5/PA6/PA7，84MHz÷32=2.625MHz |
| SPI1 RX/TX | DMA2 Stream2 / Stream3，均为Channel3 |
| ADC1 DMA | DMA2 Stream0，独立保留 |
| DRV及可选SPI编码器 | SPI3 PC10/PC11/PC12，保留原配置和DMA1 |
| DC_CAL | PB7，配置输出前先写低，正常保持低 |
| GATE | PB12，经外部 AND 与 STOP 生成 EN_GATE |
| ABZ | PB4 / PB5 / PC9，原增量编码器路径 |

独立 `Board/rehab_v13` 保存板级初始化，原 `Board/v3` 和 `Board/rehab_v12` 未改动。
V1.3 同时定义 `REHAB_V12`（既有公共接入ABI）及 `REHAB_V13`（实际板型）；已有 AS5600 I2C 硬件路径保持排除，避免占用 PB7。
ADC 初始化只操作 PA1、PB0、PC0/PC1/PC5，重初始化不将 SPI1 的 PA5/6/7 改回模拟模式。
冷启动先将 GATE/DC_CAL 输出锁存为低，再完成定时器句柄初始化，随后订阅并检查 STOP，最后启用 EXTI。

SPI1 与 SPI3 独立管理队列、DMA缓冲、超时、错误和恢复；回调依据句柄及 DMA Parent 分派。
仅接受已确认的 stream/channel 组合；非法映射拒绝事务，不将未知映射默认为其他 DMA 中断。
单总线恢复只清理自身中断及寄存器，不影响另一条总线或 ADC。

## STOP 与驱动恢复

STOP 两沿都锁存停止并作废旧命令，先调用原 `Motor::disarm()` 关闭PWM，再失效驱动就绪状态并拉低GATE。
短低脉冲即使在ISR进入前恢复，仍视为可能重置过DRV。ISR不执行SPI、延时或RTOS等待。
重复低电平轮询幂等，新边沿总是推进驱动代次。

独立 `V13GateDriver` 组合原 `Drv8301`，不修改原驱动私有成员。恢复仅在任务态、STOP高、PWM关闭、轴处于启动或待机状态时进行：

1. GATE低至少40µs。
2. 通过原公开配置接口依次选择10、80及目标增益，强制原缓存失效并还原目标配置。
3. 检查STOP/代次后拉高GATE，执行原初始化和寄存器写入、回读。
4. 再次检查STOP、代次及PWM；不一致时拒绝就绪并保持GATE低。

普通待机不重复硬复位，保留电流偏置采样条件。物理STOP恢复只允许重新准备驱动，不自动清错、不恢复运动。
CLEAR、ENABLE、START仍是独立操作。原FOC、控制器和编码器算法没有因本次迁移而改动。

## 制动两条路径

V1.3 网表中 U12 HS 已连接实际半桥中点，V1.2 的 HS 未连接缺陷在该版本网表中已修正。
这不代表实体制动验收；实体连通、自举供电、切换时序、回生能力及温升仍待测量。

| STOP | DELAY | 选择器行为 |
|---|---|---|
| 高 | 高 | MCU AUX_H/AUX_L控制HI/LI |
| 高 | 低 | 输出禁止 |
| 低 | 高 | 输出禁止 |
| 低 | 低 | HI由555输出控制，LI为低 |

**软件关闭制动PWM不能关闭STOP低电平下的555硬件制动支路。** 软件不强行覆盖该逻辑。
诊断 `brake_settled_path` 依据STOP推断DELAY稳定后的预期路径：1=MCU、2=555；没有测量DELAY或HI/LI，不能确认切换瞬态。
`software_brake_enabled` 和 `hardware_stop_brake_present` 分开报告；前者为0不代表硬件制动不工作。

## 温度、配置和诊断

R25/R66采用已确认的 HNTC0805-103F3950FB：名义10kΩ，B25/50=3950K；上臂NTC、下臂3.3kΩ。
`Rntc=3300*(1-ratio)/ratio` 后使用Beta公式；开短路、近电源轨及非有限输入返回无效温度。
名义参数只用于换算，实测降额/停机阈值、母线阈值、制动/回生、电流极性、电机编码器和时序验收仍独立锁定。

原板、V1.2、V1.3的原生配置版本为 `0x0001`、`0x1201`、`0x1301`，CRC seed隔离，拒绝直接跨版加载。
业务配置继续只存RAM。原framed命令编号及诊断字0..62保留，V1.3在63..80追加板版本、双SPI、驱动就绪/代次和制动路径，浮点10追加制动温度。
原SPI诊断21..24、35在V1.3对应网络SPI1，新增66..70对应原生SPI3。

## 历史保护与验证边界

原始 `analysis/rehab_v12_baseline` 及旧 `core_manifest.json` 保留。实施前提交 `0509745812bb790b5931e3273a760347b137db86` 已完整存档到 `analysis/rehab_v13_baseline`。
它含已有AS5600等合并变化，并非未修改的上游0.5.6。36项历史差异逐项记录，其中15项超出旧接入范围；本次保留，不通过扩白名单掩盖。
V1.3对现有核心文件仅修改GPIO中断测时、SPI仲裁头、NVM版本及Tup构建接入；其余实现位于独立扩展和板级目录。
具体审查及测试结果见 [V1.3审查记录](v13_review.md)。

主机测试使用实际运行层、仲裁器和栅极包装器，HAL/外设由测试载体提供。板级契约是生产源码映射检查；ARM构建验证真实头文件和链接，不能证明电气时序。
实机仍待验收：GATE/PWM截止时间、DRV寄存器恢复、ADC同步与数值、SPI波形、制动切换、回生温升及温度阈值。

## 原厂依据

- [ST RM0090：STM32F405 DMA映射](https://www.st.com/resource/en/reference_manual/dm00031020-stm32f405-415-stm32f407-417-stm32f427-437-and-stm32f429-439-advanced-arm-based-32-bit-mcus-stmicroelectronics.pdf)。
- [TI DRV8301：EN_GATE复位及DC_CAL](https://www.ti.com/lit/ds/symlink/drv8301.pdf)。
- [TI SN74LVC157A：选择器真值表](https://www.ti.com/lit/ds/symlink/sn74lvc157a.pdf)。
- [NTC原厂规格书（立创托管）](https://atta.szlcsc.com/upload/public/pdf/source/20250801/76FA921A193BC694B25BBEA51F4DE71C.pdf)。
