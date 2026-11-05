# V1.3 原生电机接口与基础外设清理

清理起点：897078d5（FOCV2.1）。本次不重置仓库，不连接设备，不烧录，不执行运动。

## 当前使用入口

在 Firmware 目录执行 `make` 构建；`make flash-check` 仅生成并校验烧录命令、不访问设备；由用户执行 `make flash` 才会烧录。
默认且唯一的 V1.3 目标为 `peripherals`，输出 `build/rehab/v13/peripherals/ODriveFirmware.elf`。
此固件允许原生电机命令，开机待机，不是“永久锁定 full”。旧 V1.3 full/training/statistics 等入口拒绝执行。

## 保留与删除

保留 V1.3 单轴 ADC/PWM/引脚、双 SPI/DMA 隔离、DC_CAL 初始化、温度换算、STOP 锁存与 DRV 恢复、原生制动和其他保护。
保留 PWM 关闭时用 DC_calib_ 作为合成样本以及读取下一份真实 DC 样本前清空 optional 的修复。
CH394 默认 192.168.11.2:39400、网关 0.0.0.0、掩码 255.255.255.0；启动自动配置。SetNetwork 只在原生轴完全待机时接受，配置只在 RAM。
保留 HX711 原始计数、序号、样本年龄、有效位和时序错误；不提供去皮、力标定或训练功能。

V1.3 不再编译 Engine、训练力控制、机械换算、统计、补偿、旧应用 Service/runtime、ISR 统计、ADC 观察器和临时故障捕获。
删除 capture/phase_adc 的专用实现、读数工具、生成电机副本脚本及对应测试；原始 MotorControl/motor.cpp 直接编译。
V1.2 仍依赖的旧运行层、Engine 和对应测试保留用于兼容，明确不进入 V1.3 镜像。
历史日志、离线研究、已有审查和两个基线档案均保留；历史文档中的捕获/台架命令不适用于此版本。

## 通信兼容性

保留命令 1 Hello、2 Status、12 Stop、18 Diagnostics、19 SetNetwork；其他编号在合法会话下返回 Unsupported，编号不复用。
Hello 的 0x1201 仍是协议标识，不是板型；板型由 rehab.board_revision=0x1301 标识。
CRC、分包/粘包、会话、重复请求缓存、队列容量与故障代次边界保留。
Status 仍是含 result 的 64 字节；训练状态/所有权/有效标志等填 0，force sample_age=UINT32_MAX，四项机械/力浮点值为 NaN，应用运动能力位为 0。
Diagnostics 仍是 result 加 63 个 uint32。删除的应用/ISR槽位返回0（样本年龄槽位7为UINT32_MAX）；实际网络/HX711/系统运行诊断保留。
Fibre 保留原有基础诊断属性（无效应用量按上述规则返回）；移除 cal_capture_arm/word、phase_adc_* 和 V1.3 SPI1 ISR 统计属性。
网络芯片故障快照和双 SPI 错误计数保留，因为它们属于外围驱动诊断。
Stop 是显式软件停止并锁存；网络断连本身不停止原生运动、不喂原生看门狗，原生看门狗由用户通过原接口配置。

## 原生参数与安全边界

原生参数读写不再经过训练控制权管理，不替用户覆写极对数、CPR、电流、R/L、电压、温度或校准状态。
保留配置版本0x1301以及禁用启动自动校准/自动闭环的启动待机策略。拒绝旧0x13B1台架配置。
**默认过压仍为0V**：V1.3编译电压标识为0，原生默认表达式为1.07*HW_VERSION_VOLTAGE。此清理不猜测24V/48V产品参数；任何测试前须用原生命令明确设置并读回电压/电流参数。
物理STOP触发后先关PWM，再由板级逻辑处理GATE/DRV失效。释放STOP及DRV恢复不会自动运动；仍需显式清错及新的原生请求。
不改变原生过流、回生、过压、DRV保护判断，不直接对相电流取反。清理完成不等于电流跳变原因已解决。

## 可重复的离线验证

```
python -B Rehab/scripts/test.py --quick --cxx D:/mingw64/bin/g++.exe
python -B Rehab/scripts/build.py baseline --jobs 1
python -B Rehab/scripts/build.py full --board v12 --jobs 1
python -B Rehab/scripts/build.py peripherals --board v13 --jobs 1
python -B Rehab/scripts/resources.py baseline
python -B Rehab/scripts/resources.py full --board v12
python -B Rehab/scripts/resources.py peripherals --board v13
python -B Rehab/scripts/check_core.py
python -B Rehab/scripts/project.py check
```

主机测试用模型验证逻辑与协议，不证明实物PWM、制动能量或电流采样电气性能。
保护登记采用逐项旧/新哈希记录；不执行全量freeze，不改两个历史基线。

## 本次验证结果（2026-11-05）

- 最终主机快速回归：47组通过、0失败、1组跳过（原版主机长测未在quick模式运行）。
- 外围协议：189项检查通过；V1.3真实运行层配合硬件替身：62项检查通过。
- 原板baseline、V1.2 full、V1.3 peripherals：ARM编译与ELF资源/模块隔离检查全部通过。
- 源码保护：694文件继承快照、两个历史档案及87个审查登记文件通过；原生算法/原板/V1.2板文件没有本次差异。
- 最终project check通过；未执行烧录或访问控制板。
- V1.3 ELF SHA256：`3f5cbc5a4ef022af7847b3b7dfdf144cae1b1ff9b28fe7d177c61c2b24e2a5d6`。
- 完整测试输出：`Firmware/build/rehab/tests/report.json`；各构建输出目录中的`build.json`、`build.log`记录编译证据。
