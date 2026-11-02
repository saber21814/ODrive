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
