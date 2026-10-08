# 验证、交付状态与回退

验证日期：2026-10-04（Asia/Hong_Kong）。主机编译器：MinGW g++ 8.1.0；
ARM编译器：arm-none-eabi GCC 7.3.1。实际命令、退出码、耗时和日志保存在下列机器报告中。
最终主机矩阵共35组，全部通过；可随源码保留的摘要见 [software-results.json](software-results.json)。

## 已执行的软件测试

入口：`python -B Firmware/Rehab/scripts/test.py`，也可在Firmware目录运行 `python -B Rehab/scripts/test.py`。
报告：[`Firmware/build/rehab/tests/report.json`](../../Firmware/build/rehab/tests/report.json)。
日志：`Firmware/build/rehab/tests/logs/`。

| 组别 | 运行的实际生产代码 | 已记录结果 |
|---|---|---|
| 核心 | Rehab/core/engine.cpp | 423125项检查通过 |
| 帧/重放 | Rehab/protocol/frame.cpp | 11场景、31959断言通过；含CRC碰撞及10000轮固定种子随机输入 |
| 业务服务与24h长测 | Engine+Frame+Service+config_fields | 37154454项检查通过；24h虚拟时间、20次微秒时钟回绕 |
| 核心开关 | 训练/统计/补偿的8种组合 | 8/8通过 |
| 业务开关 | 应用/训练/统计/补偿的16种组合 | 16/16通过 |
| 真实驱动模型 | SPI arbiter+HX711+CH394Q | 730项检查通过 |
| 固件运行期适配 | 实际runtime.cpp及真实Fibre Property入口，替换HAL/RTOS/承载对象 | 16场景、124项检查通过；默认及REAL_MOTOR=1两版均保持硬件锁 |
| USB/UART流生命周期 | 实际Stream与Fibre异步接口 | 300项检查通过；含63B分片及重连后的迟到RX/TX回调 |
| 原生配置版本隔离 | 实际ConfigManager、内存NVM后端 | 原板/V1.2各5场景397检查通过；同长度同payload异版本拒绝、截断、单bit损坏、中断写 |
| 原生主机回归 | 原仓库Tests/*.cpp | 25用例、1433285断言通过；明确跳过1个step_cb超长循环用例 |
| CLI物理替身 | 实际Engine+模拟质量/绳索/传感器/执行器 | 60秒运行和正常卸力退出通过；CSV输出 |

原有 `step_cb` 测试约86亿次循环，本次未执行。运行期测试验证训练控制权下step-dir接管拒绝；
不能把它描述成原有超长 `step_cb` 测试已经通过。
原生Tests中的轨迹测试本来就含测试内旧实现，因此本报告也不把这些主机用例当作FOC实机验证。

24h场景获得864000个10Hz力样本、21599次完整往复，累计时间86400秒，分方向能量与独立参考积分
相对误差小于2ppm；随后注入STOP，验证过期START不能清锁，CLEAR后仍不使能。
这段测试运行实际应用和协议，不包含真实电气能量转换或机械辨识。

驱动模型注入启动失败、DMA错误、队列/活动超时、迟到回调、回调重入、DMA EN卡住、BSY异常、
IRQ内模式切换延后到任务、HX卡高/卡低/时钟停转、10/80SPS、CH394初始化回读/环形缓冲/重连/超时。
模型不是STM32硅片或CH394Q实物认证，电气/IRQ时限仍须实测。
NVM模型也不替代实际Flash断电、擦写寿命和异常恢复测试；新旧版本通过既有CRC种子机制隔离，没有声称增加Flash格式头。

运行期回归包含原生写者交接竞争、真实Property写门禁、指定只读函数参数、输入清零交还原生、
STOP锁存同位竞争、旧epoch/目标超期、最终转矩方向与幅值、断连清队列、USB/UART响应唤醒。
CAN、ASCII、PWM/模拟映射及板级启动还经过源码审查与ARM构建；未声称已经通过真实总线负载测试。

## ARM构建与资源

入口：`python -B Firmware/Rehab/scripts/build.py all`，顺序构建9个profile。
资源检查：`python -B Firmware/Rehab/scripts/resources.py all`。
完整实际表见 [`resources.md`](resources.md)，详细段地址与DMA符号见
[`resources.json`](../../Firmware/build/rehab/resources.json)。各profile独立目录中有ELF/HEX/BIN/MAP和build.json。

资源检查会验证：V1.2 endpoint无axis1、原板保留axis1；HX/网络模块符号与profile匹配；原板无Rehab运行期；
SPI arbiter/ADC缓冲位于可DMA访问SRAM；各段在Flash768KiB、SRAM128KiB、CCM64KiB预算内。
CCM的64KiB是静态FreeRTOS堆预留，任务栈从中分配，不能再把栈大小累加为额外SRAM。
SRAM统计包含链接脚本C堆/主栈预留。真实空闲堆、各任务最低栈余量、IRQ完整WCET尚未测量。

本次原板回归 size 为 text342512、data4072、bss142888 字节，与实现前原板基线一致。
这是编译体积一致，不宣称整份ELF二进制相同（版本/构建元数据可能变化）。

脚本不调用整套发布流程，不重写主机enum/DBC/Arduino生成文件。构建中出现过Windows tup临时文件
并行访问失败；脚本现使用串行job，之后以最终成功日志为准。失败镜像不会被资源脚本当作当前成功产物。

## 交付状态

| 子项 | 实现 | 软件验证 | 实机 |
|---|---|---|---|
| 单轴板、定时/ADC白名单、USART2、NVM隔离 | 实现完成 | 源码审查及ARM构建 | 实机待验证 |
| 所有权、原生门禁、快路径锁存、目标代次/期限 | 实现完成 | 核心/运行期/Fibre模型、接入点审查 | 实机待验证 |
| HX711、SPI超时/取消、CH394Q静态IPv4 TCP | 实现完成 | 真实驱动源码模型 | 实机待验证 |
| 版本帧、组帧/CRC/重放、业务会话/配置 | 实现完成 | 协议/服务组合与故障注入 | 实机待验证 |
| 等张/阶段/速度上限阻力/弹性/水阻、暂停/回程 | 实现完成 | 实际应用核心场景及CLI仿真 | 实机待验证 |
| 统计/RFD、体能流程、摩擦/惯性限幅 | 实现完成 | 参考场景及24h长测 | 实机待验证 |
| Release训练模式 | 明确返回Unsupported | 拒绝行为通过 | 不宣称原机算法复现 |
| 真实电机使能、制动与回生能力 | 默认锁定 | 两重锁定测试通过 | 硬件阻断，待验收 |
| 上位机界面、下载、电路修改 | 本次范围不包含 | 未执行 | 未执行 |

没有条目标记“实机通过”。缺失参数和制动HS网问题见[实机清单](commissioning.md)。

## 基线保护与回退

`analysis/rehab_v12_baseline/` 保存：

- `manifest.json`：570个原跟踪路径的实施前SHA256；缺失路径记录null。
- `worktree.zip`：包含原有本地修改的实施前快照；`head.zip`、`upstream-fw-v0.5.6.zip`：提交/上游快照。
- `preexisting.patch`、`preexisting-status.txt`：原有修改；`SHA256.json`：快照/补丁/清单校验。

原有 `Firmware/tup.config.default` 删除和 `tools/create_can_dbc.py` 修改均保留。
`Firmware/Rehab/scripts/check_core.py` 检查全部基线路径；仅17个固定接入文件及新板/扩展/测试/文档目录
允许本项目改动。最终审查哈希位于 `Firmware/Rehab/core_manifest.json`；任何未经重新审查的核心接入
或板级文件变化都会使检查失败。`--freeze` 仅用于人工审查后的更新，不在一键构建流程里自动执行。
原生FOC、电流环、SVPWM、控制器算法实现和 `Board/v3` 保持基线哈希。

软件回退优先只切回原板构建profile，以验证原板逻辑；**不能将原板双轴镜像烧到V1.2板上**。
源码回退时：

1. 另存当前改动和需要保留的新工作，校验baseline的SHA256。
2. 在独立目录解压 `worktree.zip`，以此作为含用户原改动的基线；不要直接执行`git reset --hard`或`git clean`。
3. 比较17个固定接入文件，逐个恢复到实施前快照；移走本次新增`Board/rehab_v12`、`Rehab`、`Tests/rehab`。
4. 保留实施前被删除的`tup.config.default`状态和已修改的`tools/create_can_dbc.py`；不要用HEAD覆盖这些原改动。
5. 重新构建对应的原板固件并核对接口，重跑原板软件检查。

V1.2原生配置版本与原板不同；回退后不会自动转换或恢复旧双轴运动配置。
