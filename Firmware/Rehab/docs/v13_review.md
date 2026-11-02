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
