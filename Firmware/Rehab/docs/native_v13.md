# V1.3 原生命令与基础外设

默认构建为现有 `peripherals`：V1.3 单轴硬件适配、USB Fibre、Rehab基础协议、CH394网口、HX711和温度采集；应用与训练控制关闭。网络地址192.168.11.2/24、网关0、TCP端口39400，电脑沿用192.168.11.10。

在Firmware目录执行：

```text
make
make flash-check
make flash
```

`flash-check`只构建并检查固件，不启动烧录器。`make flash`写入`build/rehab/v13/peripherals/ODriveFirmware.elf`并校验。默认固件具备原生命令使能能力，启动保持IDLE，不自动校准或运动。需要明确构建锁定full时使用`python -B Rehab/scripts/project.py build --profile full`。原板入口仍为`make -f Makefile`，不能将原板固件烧到V1.3。

台架阶段许可、台架CLI及台架构建已退役。命令22～26不复用，返回Unsupported。后续电机测试直接在odrivetool `In [...]`窗口通过`dev0`原生接口进行。

固件不替用户固定电机极对数、CPR、R/L、转矩常数、电流、校准电压或24V测试阈值。读取出的配置值不是测量结果。新电机须重新测R/L，不使用旧电机通过标志；无编码器时极对数仍未确认，电角速度不能冒充机械转速或推杆速度。

首次连接只读：

```python
print("board:", hex(dev0.rehab.board_revision))
print("stage:", dev0.rehab.stage)
print("axis_state:", dev0.axis0.current_state)
print("armed:", dev0.axis0.motor.is_armed)
print("gate_ready:", dev0.rehab.gate_ready)
print("STOP:", dev0.rehab.physical_stop)
print("VBUS:", dev0.vbus_voltage)
print("UV:", dev0.config.dc_bus_undervoltage_trip_level)
print("OV:", dev0.config.dc_bus_overvoltage_trip_level)
print("motor bus limits:", dev0.axis0.motor.config.I_bus_hard_min,
      dev0.axis0.motor.config.I_bus_hard_max)
print("system bus limits:", dev0.config.dc_max_negative_current,
      dev0.config.dc_max_positive_current)
dump_errors(dev0)
```

默认电机级母线限值恢复原生负无穷/正无穷，系统级负向母线限值仍为原生-0.01A。保护参数必须按实际供电、制动和电机确认后由命令显式设置并读回。不得用清错循环、无限阈值或旧的预校准标志掩盖故障。没有有效过压等参数时先配置再校准，不能以“原生默认”代替本机参数确认。

原生短时开环可使用LOCKIN_SPIN公开配置与状态请求；它仍有原生校准、方向和硬件就绪前提。每次运行采用单独明确的命令，并以IDLE退出。USB可通过`dev0.axis0.requested_state = 1`请求停止。网络基础查询不会持有电机控制权；TCP断开不代表原生运动看门狗，若需要断连停机应使用原生看门狗并按其控制源维护。

物理STOP、DRV恢复、ADC映射、双SPI隔离等板级功能保留。保留诊断不表示已完成实机急停、采样准确度、制动回生或行程保护验收。无编码器和限位的推杆不能自动判断行程端点。
