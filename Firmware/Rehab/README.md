# ODrive Rehab V1.2 固件扩展

新增独立 **V1.3** 板级目标，使用 `build.py <profile> --board v13`。调试步骤、STOP恢复、双SPI及实机边界见 [V1.3迁移与验收](docs/v13_migration.md)。不带板型参数仍保留V1.2构建。

基线：`fw-v0.5.6-4-gf9b5419c` / `f9b5419c820c6fb2f609322a5131cdf8c5f71657`。
代码直接集成到本仓库，原板构建保留；训练核心、外设及协议使用独立目录。
没有开发上位机界面，没有下载固件、驱动电机或修改电路。

**所有交付的 V1.2 镜像都禁止真实电机和制动 PWM 使能。**
`platform/commissioning.hpp` 的硬件确认项尚未完成；`build.py` 始终设置 `REHAB_REAL_MOTOR=false`。
只改这个编译开关也不能解锁：未确认的硬件条件仍会阻止使能。
仿真模型的参数只在主机测试中使用，不能作为 750 W 电机或真实机械参数。

## 快速使用

VS Code 的默认“build”任务现在直接执行 V1.2 full 单任务构建。
代码分析使用实际 ARM 编译命令和独立的单轴生成头文件，主机测试使用各自的 MinGW 命令；
不会通过递归 includePath 把测试替身头文件带入固件。
完整构建自动更新 `build/rehab/editor/compile_commands.json` 和生成头文件快照，之后构建原板不会覆盖编辑器的 V1.2 接口。
更换编译器安装位置后，在 full 构建成功后运行 `python -B Rehab/scripts/editor.py --configure`。
原来的 VS Code 配置备份位于 `build/rehab/editor-before/`。

在 `Firmware` 目录，用现有 ARM GCC、tup、Python 3（PyYAML、Jinja2、jsonschema）和主机 g++ 8 或更新版本：

```powershell
python -B Rehab/scripts/check_core.py
python -B Rehab/scripts/test.py
python -B Rehab/scripts/build.py full
python -B Rehab/scripts/resources.py full
```

全部阶段的一键验证：

```powershell
python -B Rehab/scripts/verify.py
```

只做主机验证：`python -B Rehab/scripts/verify.py --host-only`。
快速迭代：`python -B Rehab/scripts/test.py --quick`，跳过 24 小时虚拟长测和原版主机测试。
脚本失败返回非零，不烧录、不扫描设备，不调用原有整套发布脚本。
构建脚本在 `finally` 中按字节恢复原 `tup.config`，不覆盖用户的主机枚举、DBC 或 Arduino 生成文件。
若进程被强制终止，持久恢复记录保存在 `build/rehab/tup-config-recovery.json`；确认旧构建已退出后，
使用 `python -B Rehab/scripts/build.py full --recover-config` 恢复配置并继续。若配置在中断后被手动修改，
恢复操作会拒绝覆盖并保留记录供比较。一次只运行一个构建进程。

仿真可独立运行，输出 CSV：

```powershell
./build/rehab/tests/bin/simulation.exe 60 > build/rehab/simulation.csv
```

模拟电机、绳索及力传感器的平台替身调用固件使用的 `core/engine.cpp`；没有另写 Python 训练算法。
模型包含被动回程、执行器延迟、传感器量化和固定种子噪声。最长单次 CLI 仿真 3600 秒；
24 小时统计、计时器回绕及故障注入由真实 Service/Engine 测试执行。

## 构建阶段与产物

| profile | 板/阶段 | HX711 | CH394Q/协议 | 应用保护 | 训练 | 统计 | 补偿代码 |
|---|---|---|---|---|---|---|---|
| baseline | 原 v3.6-56V | — | — | — | — | — | — |
| native | V1.2 / 1 | 关 | 关 | 关 | 关 | 关 | 关 |
| hx711 | V1.2 / 2 | 开 | 关 | 关 | 关 | 关 | 关 |
| network | V1.2 / 2 | 关 | 开 | 关 | 关 | 关 | 关 |
| peripherals | V1.2 / 2 | 开 | 开 | 关 | 关 | 关 | 关 |
| protection | V1.2 / 3 | 开 | 开 | 开 | 关 | 关 | 关 |
| training | V1.2 / 4 | 开 | 开 | 开 | 开 | 关 | 关 |
| statistics | V1.2 / 5 | 开 | 开 | 开 | 开 | 开 | 关 |
| full | V1.2 / 6 | 开 | 开 | 开 | 开 | 开 | 开 |

`python -B Rehab/scripts/build.py <profile>`；`all` 顺序构建以上九项。
安全任务和真实使能门禁在所有 V1.2 阶段保留；关闭外设不创建其业务任务或初始化其外设。
共享 SPI3 为 DRV8301 保留。编译补偿代码不等于启用补偿，运行 flags 默认仍为 0。

- `build/rehab/<profile>/ODriveFirmware.{elf,hex,bin,map}`：各阶段独立镜像。
- 同目录 `endpoints.hpp`、`build.log`、`build.json`：实际接口、编译日志、源码/产物 SHA256。
- `build/rehab/tests/report.json`、`logs/`：命令、编译/运行退出码及测试输出。
- `build/rehab/resources.{json,md}`：实际 ELF 内存分区及模块、单轴、DMA SRAM 检查。

源文件变化会令旧资源报告检查失败；失败构建不会被旧 ELF 冒充成功。
原板 `baseline` 仅用于原版回归，不适用于 V1.2 硬件。

## 文档入口

- [原计划逐项核对、本轮补齐及实机边界](docs/plan-audit.md)
- [架构与接入点](docs/architecture.md)
- [二进制协议与配置](docs/protocol.md)
- [硬件证据、使能锁和实机验收](docs/commissioning.md)
- [验证结果与回退](docs/verification.md)
- [板级引脚及定时资源](../Board/rehab_v12/README.md)
- [外设协议、SPI 取消及驱动模型边界](drivers/README.md)
- [真实核心测试和仿真模型](../Tests/rehab/README_core.md)

业务使用顺序：HELLO → 下发配置并 COMMIT → ACQUIRE → 去皮/标定或查询 → ENABLE → START。
当前真实镜像的 ENABLE/START 返回 `Inhibited`，软件仿真具有独立的模拟准入条件。
PAUSE 受限卸力后失能，保留控制权；RESUME 必须重新检查。
FINISH 正常结束，失能后 RELEASE 交还原生接口；STOP 立即锁存并走原生 disarm。
CLEAR、ENABLE、START 是独立命令；断连、清错和重启不恢复旧目标。
`Release` 训练模式始终返回 `Unsupported`，不宣称复制未取得的原机算法。
