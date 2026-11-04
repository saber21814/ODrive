# V1.3 迁移审查记录

日期：2026-10-29。实施前用户基线：`0509745812bb790b5931e3273a760347b137db86`。
用户基线包含既有AS5600及其他合并代码，不称为未修改上游0.5.6。
范围和复现命令见 [迁移与验收](v13_migration.md)。

## 独立代码审查及修复

板级适配与SPI仲裁分开实现，由未参与这两部分实现的审查角色检查实际源码及diff。
审查覆盖中断/DMA/RTOS交互、共享状态临界区、STOP代次和短脉冲、栅极恢复、普通待机、GPIO初始化、ADC重初始化、配置/协议兼容及历史保护。

| 发现 | 修复 | 回归证据 |
|---|---|---|
| STOP启动为低时，early_init在TIM句柄初始化前调用原disarm，存在空指针风险 | V1.3 early_init移至所有MX初始化之后、EXTI开启之前；GPIO先保持GATE低 | v13_board_contract检查冷启动顺序；真实ARM链接 |
| 核心保护仅扫描untracked，暂存新增核心文件可能绕过检查 | 对cached及others全集检查，使用NUL分隔保留中文路径 | core_protection_v13测试注入暂存核心新增路径、算法/V1.2/DRV及历史证据变化，全部拒绝 |
| 网口关闭阶段仍初始化SPI1及其DMA中断 | 专用SPI1初始化和DMA2 Stream2/3 IRQ由REHAB_ENABLE_NETWORK守卫 | 板级契约及native/network/full构建 |
| TX错误中断可能在RX完成检查与成功提交之间抢占，错误被误报成功 | finish提交同一临界区再次检查pending_error；错误保留给task service恢复及失败回调 | 两条SPI分别精确注入窗口；修复后863项驱动检查通过，移除修复行后新增测试确实失败 |

审查未发现其他确定的阻断缺陷。源码审查和主机模型不证明实体电路通过验收。

## 继承差异单独审计

原始 `analysis/rehab_v12_baseline` 与 `Firmware/Rehab/core_manifest.json` 保留，未刷新旧哈希。
新增 `analysis/rehab_v13_baseline` 保存本次实施前694个跟踪文件的原始字节、归档SHA256及历史差异。
对旧基线共有36项继承差异，其中15项超出旧接入列表。它们在本次迁移中逐字节保持不变：

| 分类 | 文件 |
|---|---|
| CI缓存版本 | `.github/workflows/documentation.yml`、`.github/workflows/firmware.yaml` |
| AS5600板级I2C | `Firmware/Board/v3/Inc/i2c.h`、`Firmware/Board/v3/Src/i2c.c`、`Firmware/Board/v3/board.cpp` |
| AS5600编码器 | `Firmware/MotorControl/encoder.cpp`、`Firmware/MotorControl/encoder.hpp` |
| AS5600接口/枚举 | `Firmware/odrive-interface.yaml`、`Arduino/ODriveArduino/ODriveEnums.h`、`tools/odrive/enums.py` |
| 既有错误清输出逻辑 | `Firmware/MotorControl/controller.cpp` |
| IDE路径 | `ODrive_Workspace.code-workspace` |
| CAN工具及生成物 | `tools/create_can_dbc.py`、`tools/odrive-cansimple.dbc` |
| 生成器正则字符串 | `tools/fibre-tools/interface_generator.py` |

本次仅确认上述保留事实及AS5600 I2C路径被Rehab宏排除，不宣称完整评估了历史AS5600功能。
V1.3既有核心接入范围仅4个文件：`stm32_gpio.cpp`（EXTI4测时）、`stm32_spi_arbiter.hpp`（仲裁对象生命周期）、`nvm_config.hpp`（版本）、`Tupfile.lua`（构建）。
原V1.2板及原DRV8301实现保持实施前字节；新增逻辑放入独立板目录、Rehab与测试目录。
新的精确审查哈希在本次审查后单独登记到 `core_manifest_v13.json`，不扩大旧列表，也不替换原历史证据。

## 验证结果

<!-- validation-start -->
完整主机测试：44组通过、0失败、0跳过；包含24小时虚拟测试和原版主机回归。
上游 `step_cb` 的2^33次长循环按既有策略排除，不能声称该单项通过；另有实际函数的有限边界/种子回归。
驱动严格编译：V1.2/V1.3宏各863项检查通过。
ARM构建：原板1个、V1.2八阶段、V1.3八阶段，共17个镜像全部通过。
两版ELF资源、模块开关、单轴/双轴接口隔离、双SPI DMA SRAM位置、源码摘要及ELF/HEX/BIN/MAP实际文件哈希核验全部通过。
V1.3 full：Flash 364464字节，SRAM静态保留85748字节，CCM堆保留65536字节；未超过链接预算。任务运行时栈/堆余量仍需实板测量。
V1.2生成接口与实施前逐字节一致，V1.3保留原73个属性顺序并追加到92个；原framed协议源码保持不变。
两版full编辑器上下文分别保存；原tup.config已恢复，没有遗留构建恢复记录。
最终源码摘要：`bf0834c01970cb15aa4d451dccdd11c16874e1ae4d9d1329383cbbacc29b758f`。
机器摘要见 [software-results_v13.json](software-results_v13.json)，V1.3资源明细见 [resources_v13.md](resources_v13.md)。
<!-- validation-end -->

日志、命令、编译器、退出码和产物SHA保存在 `Firmware/build/rehab/`；失败或源码摘要不一致的构建不作为成功证据。
过程中一次构建因竞态修复改变源码而被摘要检查拒绝，随后针对最终源码重新构建。
另有V1.3网口及V1.2保护阶段各一次编译器内存不足；失败日志保留在相应产物目录的 `attempt-oom.log`，串行重试后全部通过，没有改动工具链或降低源码/资源校验要求。

## 实机待验收

PCB DRC接口未完成验收；GATE/PWM关闭延迟、DRV实际寄存器恢复、ADC同步与零偏、SPI/DMA波形、制动切换、回生温升及温度阈值均待实测。
软件关闭制动不关闭STOP低电平下的555支路。V1.3 HS网表连线已修正，但不代表实体制动验收通过。
真实电机默认关闭，全部准入声明保持未验收；没有烧录、运行电机或修改电路。

## 2026-11-02 构建与烧录入口独立复核

原因：默认 make flash 先按恢复后的 v3.6-56V 配置重新构建；VS Code build 未指定板型时还会落到 V1.2。
新增 GNUmakefile 优先入口固定 V1.3 full；project.py 在调用 ST-Link 前检查保护、板型、profile、使能锁、源码摘要、产物哈希及真实 ELF 资源/接口，读回校验成功后才复位运行。原 Makefile 保留显式 -f 入口。

独立审查者 flash_entry_review 对实际差异复核通过；其提出的保护测试缺少完整模式基线断言问题已修正，并增加遗漏登记反例。主控 main.cpp 仅恢复误删首行，恢复后与原归档逐字节一致；FOC、编码器、控制器、外设中断实现未改。原始归档和旧清单保留，未运行整体 freeze。

仅对下列 8 个文件登记新哈希，其余已有哈希不变。新增 4 个项目入口是固定名称、精确哈希保护；不扩大算法文件修改范围。测试文件位于原有测试扩展目录。

| 文件 | 原登记 SHA256（— 表示本次新增登记） | 本次 SHA256 |
|---|---|---|
| `Firmware/GNUmakefile` | `—` | `b30518d6b4c52dbadfbeea15be5d0fbf51db7eb3e1081b7fab3589cbff118d2b` |
| `Firmware/.vscode/tasks.json` | `—` | `b7b4bead78b9a5358f06e40df781d7960641b52f2746ea02eb99e6d0239a66a3` |
| `Firmware/.vscode/launch.json` | `—` | `9c8b47613351fe1c911e34992d591470bfeb45b9ff23ecaecdae55800a52f0dd` |
| `Firmware/.vscode/c_cpp_properties.json` | `—` | `50d5d09d37f2a6b599a8eb7e7363b3540de19c0608243a032414f2f0b11c826b` |
| `Firmware/Rehab/scripts/project.py` | `—` | `f750cb202dee960baf12a0f8cc0364b210aa25613e7ee997c66c0e9fc77be428` |
| `Firmware/Rehab/scripts/core_protection_v13.py` | `e49043556f0e064cd7d1ce11c427fa9f1d09f3305869b3aadfcd6012fce20cc3` | `8ed24ce8aaf8ccf34f5017db2dbc5534e4f8c8fd05bceeaf0c4b180c10c3626b` |
| `Firmware/Rehab/scripts/editor.py` | `e1fb3d26a3aa95e34277db42ad457f60796ad4eeb96e94224c488ff2985d9bd0` | `0452185a896a1886bbd549f294627c8b92795f5cb7590b13a61fd208513b145c` |
| `Firmware/Rehab/scripts/test.py` | `ee71c7aa6f876c2fc7b1c48cd18dd8a2b901f7b6939d99345e43ec56dc18f3ef` | `e241ab515e9cf7bedda92ec68261936eb9daa5c2071c495a0584b4155ed179d4` |

此登记只表示静态代码审查通过。验证结果见 v13_migration.md；本轮不连接硬件、不烧录、不使能电机。

## 2026-11-02 待机队列与网口配置修复

实板 SetNetwork 返回 InvalidConfig：原生显式 IDLE 会形成 [IDLE, IDLE, UNDEFINED]，扩展层原先只接受尾项立即为 UNDEFINED。现在只读扫描活动队列，允许连续 IDLE 至首个 UNDEFINED；任何活动校准/运动任务、待处理请求、已使能、齿槽校准或缺少终止符仍拒绝。不改原生状态机、FOC、控制器、编码器或电机准入锁。

独立审查 idle_queue_review 通过；六组针对性主机回归通过，包括 V1.2/V1.3、真实电机标志两种组合（准入仍锁定）、实际二进制 Hello→SetNetwork 路径以及 native_ingress。CH394 为断连测试替身，不代表物理网口验收。原45组历史结果保留；新增日志位于 build/rehab/idle_queue_fix_20261102。

仅更新以下两个已登记文件的精确哈希，未扩大范围、未刷新原始归档。测试及测试替身位于原有测试目录。默认过压阈值问题不在本次修改范围。

| 文件 | 原登记 SHA256 | 本次 SHA256 |
|---|---|---|
| `Firmware/Rehab/platform/runtime.cpp` | `b933031675b54cfae7946bd38110f8d42faf1f57ce6b3c0623254d64d71a04e3` | `cb6bfb2317fd3ec90490f497f10124a5666b578002d594d897f86efe870733a4` |
| `Firmware/Rehab/scripts/test.py` | `e241ab515e9cf7bedda92ec68261936eb9daa5c2071c495a0584b4155ed179d4` | `aa4d8e67cabefda6a15a011877bad3c31c7d4092aba6a3738089701116d40293` |

V1.3 full ARM 构建及 project.py flash --dry-run 核验通过：text=359000、data=5484、bss=145856；ELF SHA256 `2acd114bc04a713c5e6bd03558fd2c2bb8d6779ab30fc8c1d0907c707fd765a0`。real motor LOCKED，未执行烧录。首次 Tup 临时文件访问失败，保留日志后串行重试通过（idle_queue_fix_20261102/tup-first-attempt.log）。

## 2026-11-03 CH394 首次命令超时快照

实板证据：OPEN 等待阶段 next=7，SPI读取 Sn_CTRL 持续返回0x01；SPI1错误/超时未增长。用户报告AVDD33/VDDIO均3.3V、25MHz波形存在、AVDDK/DVDDK均恒定1.2V。这些记录不等于硬件已全面验收，根因尚未确定。

V1.3首次CommandTimeout后、拉低复位前，由原网络任务执行5次有界只读SPI传输，锁存Socket前6字节和MODE/PHY_CFG/CHIP_CONFIG/REG_PAGE。部分读取失败按有效位区分；不伪造成功，不覆盖原始错误。快照跨重试保留，重新下发有效网络配置后清零。100ms判定不变；第一次故障复位前增加有限采集延迟，采集后重新开始250ms复位等待。没有在ISR、Fibre getter或关中断区执行SPI。

独立审查ch394_snapshot_review通过；10组针对性主机回归全部通过。V1.2生成接口逐字节一致，V1.3原92属性顺序和值保留，尾部新增13只读属性；二进制Diagnostics仍为结果字加63诊断字。历史结果保留，新增日志位于build/rehab/ch394_snapshot_20261103。原生电机算法与准入锁未改。

仅对以下已有登记文件更新精确哈希，未使用整体freeze，原始历史归档不变。

| 文件 | 原登记 SHA256 | 本次 SHA256 |
|---|---|---|
| `Firmware/Rehab/drivers/ch394.cpp` | `ca39335061a5338aa892e74d56888afc6d986f36c24aaad07770b540ceffa20a` | `753a7b8e4bcde16ff1a66387c33e89cf0a9ea8831827f3012f745b021f981673` |
| `Firmware/Rehab/drivers/ch394.hpp` | `33b6f85a7c86e54479fddd0d1d33523033109ab20403d9086e8a4e7b856bd9fb` | `2e49fcfff21e0ccfe9b49e2dd725296161f2f17c5cb08cd2de00bea46ab7eea9` |
| `Firmware/Rehab/interface.py` | `a328fc7ab7aa858b9ea9001f2f332b6b6184a8c2b4c05d8e4c4e37a965777cc7` | `800d4ccf7430101ea2e9b0129f4b92ca5aa83f3d4cc99df2f3ee776445b4db2e` |
| `Firmware/Rehab/platform/diagnostics.hpp` | `eb4f9b5ebac2c41b08ca820b208cf4f5ea77ef8ca0b01ea465d1ac19886eb8df` | `61d517fb2c39e497583d7f1b381f33d38a79a98df5711266a4b749ce7cdcba52` |
| `Firmware/Rehab/platform/runtime.cpp` | `cb6bfb2317fd3ec90490f497f10124a5666b578002d594d897f86efe870733a4` | `4e447bc11b10cbb4d4ecf38e053ad9fb6023f130ef4a77ec38fc8fd0f68aee3a` |
| `Firmware/Rehab/scripts/test.py` | `aa4d8e67cabefda6a15a011877bad3c31c7d4092aba6a3738089701116d40293` | `858a1422c7598a2d7a18dd17b2523c1986c32306c091afaed2a8d1f60a30fa58` |

V1.3 full ARM构建及project.py check通过：text=362100、data=5484、bss=145960；ELF SHA256 `455fa2fcc869e6254ef21663ee76dcdb862d55c3cb562cfaf793911245cc4b79`。电机准入锁仍关闭使能；本轮未烧录、未接触硬件。诊断版改变对象布局，旧固定SRAM地址脚本不可复用。

## 2026-11-03 CH394 OPEN等待时间软件对照（临时诊断）

实板首次快照valid=0x1f、OPEN=1未清零、Sn_STA=0、TCP模式=1、端口39400、PHY_CFG=0xb8；用户测得RSTB高约3.3V持续170ms、低约250ms，无提前跌落。用户无法测SPI波形，改用软件单变量对照，尚无根因结论。

仅V1.3且REHAB_REAL_MOTOR=0时，WaitCommand的OPEN截止从100ms改为2000ms；任务逐次poll，未增加阻塞循环，其他命令、WaitOpen、SPI速度、初始化顺序、250ms回退均不改。V1.2及真实电机标志路径仍100ms。快照接口/布局、原生算法和电机准入锁不改。诊断完成后恢复原截止时间。

独立静态审查ch394_open_deadline_review未发现阻塞问题。三组主机模型通过：V1.2、V1.3锁定及V1.3真实电机标志分支（仅模拟）；覆盖截止边界、uint32回绕、延迟OPEN完成、其他命令截止及既有快照测试。日志build/rehab/ch394_open_deadline_20261103。

仅更新以下一个原已登记文件的精确哈希，不扩大范围，不刷新原始归档；上述为诊断对照，不代表电气或电机验收。

| 文件 | 原登记 SHA256 | 本次 SHA256 |
|---|---|---|
| `Firmware/Rehab/drivers/ch394.cpp` | `753a7b8e4bcde16ff1a66387c33e89cf0a9ea8831827f3012f745b021f981673` | `9b1a632f36a1c181a79f446d74dc2800af32208618a5b154ffe9f4829d8a5b0b` |

ARM full编译及project.py build核验通过：text=362132、data=5484、bss=145960；ELF SHA256 `d6d003c70e4dc286f539a2f1f38e9602fb35d03b905105d7f828176c23b958d4`。real motor LOCKED，未烧录。原100ms镜像备份于build/rehab/ch394_open_deadline_20261103/previous_flashed.elf，原实板结果保留。

## 2026-11-03 CH394 PHY先行初始化软件对照

2秒OPEN实板仍超时（Sn_CTRL=1/Sn_STA=0），但快照PHY_CFG=0xbf。Windows12秒采到7次Up，100Mbps连接后约0.7至0.9秒断开，循环约2.4秒，与故障复位周期接近；不能将快照link当作持续链路通过。

参考WCH作者V1.0.1（2024/12/09）CH394Q_TCP_SERVER/main.c先GetCHIPV、PHY_Check等待link，再InfParam/SocketBuf/ServerInit。官网直接下载未成功，使用公开镜像原厂署名源码（未执行），来源/压缩包SHA及选取原文见build/rehab/ch394_wait_link_20261103/reference/provenance.json。镜像不代表已核实为最新版。例程还存在500ms复位等待、CLOSE等差异，本轮不同时修改。

仅V1.3锁定构建Identify后进入追加WaitLink=10，单次poll一次有界PHY读取，LINKS=1才进入原配置。无链路最多10000ms，追加LinkTimeout=8后走原复位回退，不生成OPEN快照。保留2秒OPEN、原速度/缓冲分配/复位及其他命令截止，V1.2和真实电机标志路径不变。原生算法与电机准入不改。独立审查ch394_open_deadline_review批准；四组主机测试通过（V12、V13、V13 real flag、runtime_network_v13），含无链路不写配置、迟到link、截止/回绕、SPI失败、deinit。

本轮是临时对照而非已确认修复，只登记两个已审查文件精确哈希，旧基线不刷新。

| 文件 | 原登记 SHA256 | 本次 SHA256 |
|---|---|---|
| `Firmware/Rehab/drivers/ch394.cpp` | `9b1a632f36a1c181a79f446d74dc2800af32208618a5b154ffe9f4829d8a5b0b` | `112d7a84951358971cd848f4d27137af696b7d1ac3310078a94b4074ec3f9291` |
| `Firmware/Rehab/drivers/ch394.hpp` | `2e49fcfff21e0ccfe9b49e2dd725296161f2f17c5cb08cd2de00bea46ab7eea9` | `4a5d6a01f687a393d7fd22a1cf078048b9d7d690a50c3408467e0d27fe242446` |

ARM full构建及project.py核验通过：text=362212、data=5484、bss=145960；ELF SHA256 `c39d646fda8d6e495e4ee460e141024566a579ec97f125aed69afc7474a19108`。real motor LOCKED；未烧录，实板结果待验。上一2秒OPEN镜像备份于build/rehab/ch394_wait_link_20261103/previous_flashed.elf。


## 2026-11-03 T2 locked bench software preparation (partial)

User reports a 0.47 ohm / 50 W brake resistor and a jumpered STOP loop. STOP physical testing is deferred by the user, not passed. Software brake stays disabled; hardware STOP-low braking is not disabled or qualified by this change. Previously passed USB, network N1-N5, 30-second combined check, HX711 raw and encoder basic results are retained. No claim of ten-minute endurance, force calibration, power-stage or motor acceptance.

V1.3 CH394 now retains the bench-validated WaitLink and 2000 ms OPEN poll deadline with either REHAB_REAL_MOTOR flag. V1.2, all other command deadlines and protocol layout remain unchanged. This does not by itself qualify networking with an energized motor.

V1.3 locked builds only: early_init replaces exactly zero native OV with 28 V for the current 24 V bench setup, after configuration loading and before ADC/PWM starts. Explicit nonzero limits remain unchanged; UV and motor/brake admission are unchanged. This is not a production voltage rating.

Independent reviewer t2_locked_review approved the actual diff against this turn's before snapshots. Three driver models (V12, V13, V13 real flag) and four runtime models passed. Sandbox compiler WinError623 attempts are retained separately. Targeted logs: build/rehab/t2_locked_prep_20261103. No hardware accessed, flashed or energized. The restricted native motor commissioning entry and ADC diagnostics are still pending; this is only the locked software preparation part of T2.

Only the following already-registered exact hashes were updated after review; original archives and all other registered hashes are unchanged. No full freeze.

| File | Previous SHA256 | Reviewed SHA256 |
|---|---|---|
| `Firmware/Rehab/drivers/ch394.cpp` | `112d7a84951358971cd848f4d27137af696b7d1ac3310078a94b4074ec3f9291` | `eb5275bae8ae38a8ae70c1877a1d630a1b85b1327d3532d9448b0a7aa0695a4f` |
| `Firmware/Rehab/platform/runtime.cpp` | `4e447bc11b10cbb4d4ecf38e053ad9fb6023f130ef4a77ec38fc8fd0f68aee3a` | `4b93164ca4300cac81e219a532ddd3244872fb89eb7476f52abda930442fe736` |
| `Firmware/Rehab/scripts/test.py` | `858a1422c7598a2d7a18dd17b2523c1986c32306c091afaed2a8d1f60a30fa58` | `b25f09ba94db8bbe8394e5af6a2d3fa7c1c52a683d0db41839c5061536cad9e2` |

ARM V1.3 full build and project.py check passed: text=362260, data=5484, bss=145960; ELF SHA256 8e83571504ad163a9074bd0bb124bf72f118a0864779fbf3094bde7ee9915ecc. Real motor LOCKED; no flashing performed.


## 2026-11-03 T2 read-only DC ADC observer

User confirmed the previous locked image (ELF 8e83571504ad163a9074bd0bb124bf72f118a0864779fbf3094bde7ee9915ecc) boots with OV=28 V without manual configuration, bus=23.925806 V, locked=1, armed=0, axis=IDLE, gate_ready=1, no errors. This startup-default repair is physically checked; prior test results remain retained.

This phase appends four read-only Fibre properties for a DC sampling observer; it does not implement or enable the native motor bench entry. The second/DC ADC acquisition supplies its actual B/C raw counts and capture-time validity/PWM flags. Failed attempts clear the old validity and accumulate errors. A single packed word keeps the raw pair and flags together; separate getters are not a combined snapshot. Age distinguishes stale sampling. No native motor algorithm, current conversion, calibration callback or power-stage permission is changed. See protocol.md for the exact bit layout.

Independent t2_locked_review approved the diff against this turn's before snapshots. Seven targeted host suites passed: V12/V13 runtime with both motor flags, network runtime, observer, and existing V13 board contract. Schema comparison proves V12 byte-identical and V13 preserves the complete 105-property prefix. Framed Diagnostics stays at 63 words. IRQ execution is bounded, but physical timing/current polarity/gain accuracy remain unverified. STOP remains deferred, not passed; brake is user-reported 0.47 ohm / 50 W and remains unqualified.

ARM full build passed: text=363396, data=5484, bss=145976; ELF SHA256 ae12e8272d5ecaaf1f28ac3695823b7ed8ffdcf41d73d5a568079c2c38cf44b4. Motor locked, no hardware access or flashing in this phase. Logs and old image: build/rehab/t2_adc_20261103. Only the following reviewed file hashes are changed/added; original history and other registered hashes are unchanged, with no whole-manifest freeze.

| File | Previous SHA256 | Reviewed SHA256 |
|---|---|---|
| `Firmware/Board/rehab_v13/board.cpp` | `b62a9cceddff16ba49a0f4cd41a27c966bcdea51ec79ce2d894e861303daabc2` | `207acffec3f0b23eec5a37f2769886eaf8c3e116d22ab9e40ba6bfa8c245b558` |
| `Firmware/Rehab/platform/board_access.hpp` | `ce0d23c8edfa13d8077f52668e55209766864e8e5dedc957823e8f0468b341f5` | `49593212bd07b422cb63ec4fedbbada1afcff57c3987c10cf15e5e71e5594100` |
| `Firmware/Rehab/platform/runtime.cpp` | `4b93164ca4300cac81e219a532ddd3244872fb89eb7476f52abda930442fe736` | `0f4bb8c435f62021234614daeab26cf0985dfb86b21aa806078d2b7bb083831e` |
| `Firmware/Rehab/platform/diagnostics.hpp` | `61d517fb2c39e497583d7f1b381f33d38a79a98df5711266a4b749ce7cdcba52` | `0a47621ac20a3213bddd0c4e40a0e55f9083fd2b6ab092b713abd8088c10d2ec` |
| `Firmware/Rehab/interface.py` | `800d4ccf7430101ea2e9b0129f4b92ca5aa83f3d4cc99df2f3ee776445b4db2e` | `aa1601d399af9d4365bdcd259cc990af515db8c6c95f8680bb247bfa06c605d4` |
| `Firmware/Rehab/scripts/test.py` | `b25f09ba94db8bbe8394e5af6a2d3fa7c1c52a683d0db41839c5061536cad9e2` | `94146819ef05d13b1d4ba28ad72c843e0e75ff435c227dda0eb8152503e57f12` |
| `Firmware/Rehab/platform/phase_adc_monitor.hpp` | `new` | `493355e2f8ebf0930b8939ab8118c56ccb2fc18a2682e5cbedaf12b21d143268` |


## 2026-11-03 Complete bounded native bench implementation

User explicitly approved a single complete code delivery before the final bench flash; no automatic flashing or motor operation. T3 physical testing is skipped at user request, not marked passed. User raw ADC evidence: B1998..2010/C2010..2019, age0..1ms,24173 additional samples, no new acquisition errors, no PWM, no native errors; this proves only raw acquisition at idle.

New independent V1.3 bench profile retains full modules and successful network setup, with production REHAB_REAL_MOTOR=0 and all production commissioning declarations still false. Explicit replay-protected commands22..26 control one bounded native phase at a time. Full/make flash remains locked; make flash-bench explicitly selects the bench image. Profile metadata states movement capability truthfully while boot remains idle. Bench NVM CRC0x13B1 rejects full0x1301 and all other versions. Bench refuses persisted bench configuration and blocks native writes; only read diagnostics and emergency IDLE remain accessible. No original FOC/controller/encoder algorithm is modified.

Native motor/encoder calibration and zero-torque/velocity/position phases have independent 500ms heartbeat, 8/15/5/10/10s absolute deadlines, staged in-boot calibration prerequisites, ADC/gate/bus/temp/numeric/speed/travel checks, and fail-closed synchronous disarm. Position setpoint is published only after native closed-loop startup. STOP/disconnect/Hello replacement/expired permission cannot resume motion. Calibration current is adjustable1..4A,0 means4A; max calibration voltage3V. Command current limits1A(zero)/3A(closed) are distinct from the fixed native6A Inorm trip envelope; the latter avoids misinterpreting existing idle synthetic offset currents as real current. Nominal temperatures and limits are provisional bench bounds, not qualified product parameters. Physical brake0.47ohm50W, STOP, current polarity/accuracy/IRQ timing and regeneration remain unqualified. Software brake stays disabled.

Independent bench_safety_review approved parent safety/integration/NVM/schema. Executor B independently reviewed Executor A build/flash/resource changes; Executor A independently reviewed B protocol/CLI. Parent reviewed the final CLI precheck alignment (-10<=temperature<50,abs(startvelocity)<=.02). Targeted tests cover actual adapter/service with modeled native states, client TCP behavior, entrypoint safety and bidirectional NVM CRC isolation, without claiming native dynamics/electrical acceptance. Test reports, reviewed-before snapshots and initial compile failures are preserved under build/rehab/bench_complete_20261103. Initial guard regression correctly refused the unregistered diff before this reviewed registration; rerun after registration is required.

Only these already-allowed integration/extension files were registered after review. Three new extension files are added with exact hashes; no modification whitelist expansion, historical archive update or whole freeze.

| File | Previous SHA256 | Reviewed SHA256 |
|---|---|---|
| `Firmware/Rehab/platform/runtime.cpp` | `0f4bb8c435f62021234614daeab26cf0985dfb86b21aa806078d2b7bb083831e` | `cdfefa0a130a00e23fd2cc107b24fa22a07f9bd43061c4769ed74051db0a447c` |
| `Firmware/Rehab/platform/bench_native.hpp` | `new` | `a6d570ed6060648fdd3d8ba7d4c3e5c09f758005c9994877a70efa0b7da77e2d` |
| `Firmware/Rehab/platform/bench_session.hpp` | `new` | `d9871876427eda57da75f2bc9daa925643a9389a5ba57902bea27ce6300f3350` |
| `Firmware/MotorControl/nvm_config.hpp` | `125a7a65311d013f181e52282ad30cee79db0b2de2fa0b409fbfb5a5e98306fd` | `dd1cb7087870492f902f6a9b922bcb289cf7cf27dd8871d85af6d162b653ad8e` |
| `Firmware/Rehab/interface.py` | `aa1601d399af9d4365bdcd259cc990af515db8c6c95f8680bb247bfa06c605d4` | `1a55365338dab103a742100f66b0bb9b3c367f50206aff4397d0142ebc76cf36` |
| `Firmware/Rehab/platform/diagnostics.hpp` | `0a47621ac20a3213bddd0c4e40a0e55f9083fd2b6ab092b713abd8088c10d2ec` | `0adcf4f6dc3cdca5c19b9f6f2eff05cb1a6f87fb5ac198bae66c1acb8c692231` |
| `Firmware/Rehab/scripts/test.py` | `94146819ef05d13b1d4ba28ad72c843e0e75ff435c227dda0eb8152503e57f12` | `f05366610317edf70a3dacf01e7966df1f7b1638363f6474562d9b8b151da797` |
| `Firmware/Rehab/scripts/build.py` | `b11a29a1fb5d49e59acc937d9389f0e1369529872c71c291c6e09e04e315f961` | `45e74d9ba5ce364e33f18cc8e91ec8d1a4203d93cc594a16e8d3a869619d0fca` |
| `Firmware/Rehab/scripts/project.py` | `f750cb202dee960baf12a0f8cc0364b210aa25613e7ee997c66c0e9fc77be428` | `9cf98c1e8964e9bcaad9be72e9cdc47dc769a575169ac553f60d45cd3565a86f` |
| `Firmware/Rehab/scripts/resources.py` | `a351db149d1aa1b40692b5e09f3834dc52ad946795dce53c717015c290a76d23` | `1f56a2e82a9a74adf7cffa02e5f7aa1e57dc707aaab2fd73857aab826f18cfee` |
| `Firmware/Tupfile.lua` | `100a7363a5cbe56d8cf4733fd50b13122977e54113a2519f8e96ed249b42009d` | `ffef970f9b1b78e4ea445b8249fdbebac315565b3a960fe29a5249131b326926` |
| `Firmware/GNUmakefile` | `b30518d6b4c52dbadfbeea15be5d0fbf51db7eb3e1081b7fab3589cbff118d2b` | `1905c5c0dade0e2b86c1b246bd087457ee9d9d6a32a9bf849433cb65a3ac2ea8` |
| `Firmware/Rehab/protocol/frame.hpp` | `5e88cb8bb8cb421cf2fb4b75eb1bd6b2e759707fabd3879fd01b69b7fc9335f3` | `867e9c1b04ce09f56b66bc0f9d5e358f277a9c80691d36a6aba72fef00067a08` |
| `Firmware/Rehab/protocol/service.hpp` | `73587a6ee3c1c53fb7c43ec9e73c72ef12743adec8567e61297ba33df55db190` | `2f0bec3fdf8013628ade11b029f3318f7c17b5506d654fe04548863ebd5d8c6b` |
| `Firmware/Rehab/protocol/service.cpp` | `a221b5d04cc469fc04c6e59778028c51b6dbcf50f61ea714a379dc93e950f62c` | `eeae5deaec1764f4eab7b5bab076e4c9d59ae33697a45d0f5d624bbcb80908d8` |
| `Firmware/Rehab/scripts/bench.py` | `new` | `ca7afe0c0b16d338d2be4c55a290ff7b7ddde1b672d6424c8677f140cb64d6dc` |

Final validation: all21 targeted host suites passed after reviewed registration; the original pre-registration failure report is retained. V1.2 full, V1.3 locked full and V1.3 bench ARM builds and resource/symbol isolation all passed. V1.2 schema byte-identical; all109 preceding V1.3 properties preserved with6 appended. Bench flash --dry-run passed without invoking OpenOCD. Delivery evidence: build/rehab/bench_complete_20261103/delivery.json.

v12_full ELF SHA256: `ca591b334530eaae11bee026979c9e351430f052e9deaafd3eee68af4b75fd97`; 352804	   5492	 144984	 503280	  7adf0	D:\00-ODrive-text\ODrive-fw-0.5.6\Firmware\build\rehab\full\ODriveFirmware.elf

v13_full ELF SHA256: `a2b09302c32517991f20ac99979dc8972b533182b3c81bf768f4cfcd83eb0948`; 364932	   5492	 145984	 516408	  7e138	D:\00-ODrive-text\ODrive-fw-0.5.6\Firmware\build\rehab\v13\full\ODriveFirmware.elf

v13_bench ELF SHA256: `b4658a976650ceb0ea5e2f5f646c700a247174872d4ba3e9b77d22f5712c1092`; 370156	   5540	 145992	 521688	  7f5d8	D:\00-ODrive-text\ODrive-fw-0.5.6\Firmware\build\rehab\v13\bench\ODriveFirmware.elf


## 2026-11-03 minimal bench transition fix and no-encoder open-loop test

User authorized the existing bench-only test additions, not a new product/profile or native algorithm change. The five production extension files below were independently read and approved by bench_safety_review. Native MotorControl tree bytes match this turn's before_hashes.json, and both immutable historical archives pass audit. No whitelist expansion or whole-manifest freeze is performed.

Adapter now tolerates only the native queue's unarmed Pending UNDEFINED window with the expected outstanding request. Actual UNDEFINED still cannot arm; entry and heartbeat deadlines remain. Stage6 calls native LOCKIN_SPIN through the existing axis task, with bounded electrical angle speed/current/time, temporary direction convention and restoration after disarm. Phase1/6 do not rely on encoder position/speed; the irrelevant native controller overspeed check is temporarily disabled and restored after disarm, without altering controller.cpp or swallowing native errors. No encoder readiness or calibration is fabricated.

Existing eight-byte BenchStart and 192-byte status stay compatible; phase1 optionally accepts explicit calibration voltage (12 bytes), stage6 requires16 bytes. Motor calibration current minimum is0.1A; legacy0/default4A remains explicit. Native6A trip envelope remains distinct from the stage6 command current cap1.5A. R acceptance follows the applied V/I envelope rather than the prior fixed0.75ohm assumption. No automatic retries, fault clearing, persistence, or hardware actions. New motor R/L must be measured again, pole pairs remain unconfirmed, and open-loop telemetry is not measured mechanical movement.

Validation at review: six targeted host suites pass (adapter3876 checks), eight affected regression suites pass, and22 Python CLI tests pass. Tests model actual extension/service code and native task-queue transitions; they do not simulate physical motor dynamics or qualify STOP/current accuracy. Bench ARM build and resources pass; full-target isolation and final registered guard results are recorded in delivery.json after completion. Evidence retained under build/rehab/minimal_openloop_20261103 and build/rehab/bench_openloop_20261103.

| File | Prior reviewed SHA256 | Independently reviewed SHA256 |
|---|---|---|
| `Firmware/Rehab/platform/bench_native.hpp` | `a6d570ed6060648fdd3d8ba7d4c3e5c09f758005c9994877a70efa0b7da77e2d` | `7250be3972a87992009f8fe46a8cebd9a7c7ecf8c21ba121c19d11ca98f0a239` |
| `Firmware/Rehab/platform/bench_session.hpp` | `d9871876427eda57da75f2bc9daa925643a9389a5ba57902bea27ce6300f3350` | `0cc5452f1a6b6980ce5eacf4585ac4cdfe9b4eeda4c6f112db8da3df5aa56a15` |
| `Firmware/Rehab/platform/runtime.cpp` | `cdfefa0a130a00e23fd2cc107b24fa22a07f9bd43061c4769ed74051db0a447c` | `6e38fe8cbf6d0d1e2b2e769fbf899f92624f710eee9bbfcd30f5e3b93ad8a87d` |
| `Firmware/Rehab/protocol/service.cpp` | `eeae5deaec1764f4eab7b5bab076e4c9d59ae33697a45d0f5d624bbcb80908d8` | `11f753c6aab7a6d14030f1877f6504cd46ab6aa8be1503995f58e21ee9d944f3` |
| `Firmware/Rehab/scripts/bench.py` | `ca7afe0c0b16d338d2be4c55a290ff7b7ddde1b672d6424c8677f140cb64d6dc` | `56ea1abe1bffda48c30b9b5c6302dc4af8c03c7642667987c212b9903ed8498a` |


## 2026-11-04 retire bench and restore native V1.3 peripheral control

User explicitly requested removal of the added bench admission and parameter overrides while retaining V1.3 hardware, USB, network, HX711 and temperature sampling. Default project/make/VSCode target is the existing peripherals profile. Only V1.3 stage2 with HX711/network/protocol enabled and application disabled admits native control; application/full commissioning flags remain unverified and unchanged.

Bench phase/lease/pass flags, native-write prohibition, fixed motor/current/voltage/temperature parameters, test CLI and build target are removed. Actual STOP/latch/DRV readiness and original native fault checks remain. Brake timer permission is static board capability so safe native CCR writes and timer initialization remain possible during STOP/gate faults. Boot explicitly requests IDLE and disables startup motion, without replacing native user motor/encoder/brake parameters. No network lease or automatic watchdog feed controls native motion.

Commands22..26 remain reserved Unsupported after session/replay validation. Prior109 V1.3 attributes and all V1.2 schema bytes are unchanged; only six appended bench properties are removed. NVM is restored to0x1301 by removing the13B1 branch; no native config data structures or algorithms are edited. All43 other MotorControl files match this turn's before snapshot. Historical logs/archives remain retained; no reset, baseline refresh or whitelist expansion.

Independent bench_safety_review approved runtime/test registration and native/STOP/brake behavior; remove_bench_build independently approved protocol/schema/NVM; remove_bench_protocol independently approved build/editor/entrypoints, including the corrected shell-safe help text. Twenty targeted offline suites pass across protocol/NVM, runtime/ingress/drivers and build-entry tests. Native runtime has76 host checks; these model interfaces, not electrical behavior. Final ARM/resource/guard results are recorded in build/rehab/remove_bench_20261104/delivery.json. No device accessed, flashed or driven during implementation.

The prior two low-current calibration attempts remain failed evidence, not accepted R/L. Native system negative-current default remains-0.01A; restoring the native motor-level infinities does not guarantee calibration success. Removal of the temporary28V fallback may expose default OV=0, which must be explicitly configured and read back for the actual24V test before clearing faults or requesting motion. Physical STOP/current accuracy/regen remain unqualified.

Only these individually reviewed hashes/deletions are registered:

| File | Previous SHA256 | Reviewed SHA256 |
|---|---|---|
| `Firmware/Rehab/platform/runtime.cpp` | `6e38fe8cbf6d0d1e2b2e769fbf899f92624f710eee9bbfcd30f5e3b93ad8a87d` | `3d39727bfd4f9a6044b3e82c73ac9281c6c4c450a558f281e694353a6c8adc04` |
| `Firmware/Rehab/scripts/test.py` | `f05366610317edf70a3dacf01e7966df1f7b1638363f6474562d9b8b151da797` | `2074cff2892d70cb0a529f55091a03d3f88dc59366365c86ba24549bf25667e9` |
| `Firmware/MotorControl/nvm_config.hpp` | `dd1cb7087870492f902f6a9b922bcb289cf7cf27dd8871d85af6d162b653ad8e` | `962a30d245fdf71b645d6e137f696cf232d422d83497ead4c305437e4577b63d` |
| `Firmware/Rehab/interface.py` | `1a55365338dab103a742100f66b0bb9b3c367f50206aff4397d0142ebc76cf36` | `aa1601d399af9d4365bdcd259cc990af515db8c6c95f8680bb247bfa06c605d4` |
| `Firmware/Rehab/platform/diagnostics.hpp` | `0adcf4f6dc3cdca5c19b9f6f2eff05cb1a6f87fb5ac198bae66c1acb8c692231` | `0a47621ac20a3213bddd0c4e40a0e55f9083fd2b6ab092b713abd8088c10d2ec` |
| `Firmware/Rehab/protocol/frame.hpp` | `867e9c1b04ce09f56b66bc0f9d5e358f277a9c80691d36a6aba72fef00067a08` | `39d50e4844839f65dbf4af1e4365244da9ba3f06b667d72b2094a9d7fa2db295` |
| `Firmware/Rehab/protocol/service.hpp` | `2f0bec3fdf8013628ade11b029f3318f7c17b5506d654fe04548863ebd5d8c6b` | `54681d9acedc69ea952f5b0789cbf529bc0b31dd06083d85d385280da2f5c946` |
| `Firmware/Rehab/protocol/service.cpp` | `11f753c6aab7a6d14030f1877f6504cd46ab6aa8be1503995f58e21ee9d944f3` | `c009a16c5af7c830ca979f2ef1297bac1a2626f06b3f018acf929e7a065792a1` |
| `Firmware/GNUmakefile` | `1905c5c0dade0e2b86c1b246bd087457ee9d9d6a32a9bf849433cb65a3ac2ea8` | `4bb8f8995c3af26198a9ca29af1daa1b5219448164c213cc692264dd9f583dc5` |
| `Firmware/Tupfile.lua` | `ffef970f9b1b78e4ea445b8249fdbebac315565b3a960fe29a5249131b326926` | `b833b852b626b22b59631f3421a7c51c62f4ecd4dea1d0fba3a92f42c597cef2` |
| `Firmware/Rehab/scripts/build.py` | `45e74d9ba5ce364e33f18cc8e91ec8d1a4203d93cc594a16e8d3a869619d0fca` | `98ab4292a62c73c089ac69eca44a3ff952acfb587d1bd202d5023bc468f87eb3` |
| `Firmware/Rehab/scripts/project.py` | `9cf98c1e8964e9bcaad9be72e9cdc47dc769a575169ac553f60d45cd3565a86f` | `f3e964f1ed635537d3fffd806ad7c0940ca990ed7f76bc2a4f16fa14194ec965` |
| `Firmware/Rehab/scripts/resources.py` | `1f56a2e82a9a74adf7cffa02e5f7aa1e57dc707aaab2fd73857aab826f18cfee` | `9b44a2506714422095b5c0511e3368c2160aef3e4587a95b7f63060ed1f02197` |
| `Firmware/Rehab/scripts/editor.py` | `0452185a896a1886bbd549f294627c8b92795f5cb7590b13a61fd208513b145c` | `b08bf8d0832ef1544fd9cd2161fc1282a9b1b83e429ec79c2793b0d9202bb1e9` |
| `Firmware/.vscode/c_cpp_properties.json` | `50d5d09d37f2a6b599a8eb7e7363b3540de19c0608243a032414f2f0b11c826b` | `89356a7a5157e2dd81bd88a67e038e4183e4f59c2ea4e0eaa0b05412f977d136` |
| `Firmware/.vscode/launch.json` | `9c8b47613351fe1c911e34992d591470bfeb45b9ff23ecaecdae55800a52f0dd` | `93f6bf8a2cdf07a5971f1a36251c8e7e369c2bdd68646c77032b99ddefa85533` |
| `Firmware/Rehab/platform/bench_native.hpp` | `7250be3972a87992009f8fe46a8cebd9a7c7ecf8c21ba121c19d11ca98f0a239` | deleted |
| `Firmware/Rehab/platform/bench_session.hpp` | `0cc5452f1a6b6980ce5eacf4585ac4cdfe9b4eeda4c6f112db8da3df5aa56a15` | deleted |
| `Firmware/Rehab/scripts/bench.py` | `56ea1abe1bffda48c30b9b5c6302dc4af8c03c7642667987c212b9903ed8498a` | deleted |


## 2026-11-04: PWM-disabled synthetic current correction

V1.3 board only: encode assumed zero as DC_calib_ before native offset subtraction. Reset current0 before the independent DC ADC fetch so invalid conversions cannot reuse a synthetic sample. No native motor algorithms, protection limits, protocols or other board targets changed.

Independent reviewer idle_current_review approved startup two-sample behavior, MOE transitions, real overcurrent protection and DC sample isolation. Host tests execute extracted production compensation/protection logic; invalid DC-fetch paths are source-reviewed, not a full HAL simulation. Hardware calibration/regeneration remains unverified.

Only reviewed board hash updated: 207acffec3f0b23eec5a37f2769886eaf8c3e116d22ab9e40ba6bfa8c245b558 -> eed88c9c0aade0099d54b521e4b130a0bdfb23154d976d0a152fe7fccd8d8d9e. Historical archives and all other reviewed hashes preserved.


## 2026-11-04: Temporary calibration cycle capture

Independent review: capture_review APPROVED. Native MotorControl source bytes unchanged. V1.3 compiles an insertion-only generated observation copy; stripping five marked blocks exactly restores native motor.cpp. No return/status, protection predicate, PWM command or calibration parameter changed. Eight generator/control-law equivalence tests and recorder tests cover freeze, ring wrap, invalid data, first event and native error fallback. V1.2 schema unchanged; V1.3 only appends two Fibre functions, framed protocol unchanged. No hardware run or flash performed. Actual real-time margin remains a bench check.

Explicit reviewed manifest delta (no baseline refresh):
```json
{
  "Firmware/Board/rehab_v13/board.cpp": {
    "before": "eed88c9c0aade0099d54b521e4b130a0bdfb23154d976d0a152fe7fccd8d8d9e",
    "after": "545fd79f18180be6de2763a014c86813387510b6b0aeb1b1da0c21f72294b15b"
  },
  "Firmware/Rehab/platform/calibration_capture.hpp": {
    "before": null,
    "after": "513b990951bf53d9d0cdcb27a0fe29c0f01034ee68fd3419f301ea3192943178"
  },
  "Firmware/Rehab/platform/calibration_capture.cpp": {
    "before": null,
    "after": "ff1dda174c916662f55c08225fbaaf0a5613136a947d075cfdbad67b75c590fb"
  },
  "Firmware/Rehab/scripts/instrument_calibration.py": {
    "before": null,
    "after": "6fbc5dbf4fe48cf544f618c19516efd6da2087efda530c11da9aa28612d205f4"
  },
  "Firmware/Tupfile.lua": {
    "before": "b833b852b626b22b59631f3421a7c51c62f4ecd4dea1d0fba3a92f42c597cef2",
    "after": "d2b9d2df7241b6c72df8fa75e3065fd045a3280c5f996facd574698f88943279"
  },
  "Firmware/Rehab/interface.py": {
    "before": "aa1601d399af9d4365bdcd259cc990af515db8c6c95f8680bb247bfa06c605d4",
    "after": "5cfe29c2b8b35819ea8773b046fd5be6e52224ab9199ce0165ba7e74d92caec0"
  },
  "Firmware/Rehab/platform/diagnostics.hpp": {
    "before": "0a47621ac20a3213bddd0c4e40a0e55f9083fd2b6ab092b713abd8088c10d2ec",
    "after": "b8eb5a8dc688724d7faf5c1942112d42bfafe31f701456fa11bf209e1e4f1335"
  },
  "Firmware/Rehab/scripts/build.py": {
    "before": "98ab4292a62c73c089ac69eca44a3ff952acfb587d1bd202d5023bc468f87eb3",
    "after": "90f6886bce2fea975947a2b9292a21d3e3e7fee9f7069701f255a63f7c58cf1e"
  },
  "Firmware/Rehab/scripts/read_calibration_capture.py": {
    "before": null,
    "after": "f25e1b5a41697f468e6d6481fd4db40003d970526590b50a95b49d81ff66236d"
  }
}
```
