# V1.2 单轴板级适配

本目录从当前工作区 `Board/v3` 的必要启动、HAL 初始化和板级运行代码独立派生，仅实例化 M0。原板目录保持独立。本目录不实现训练算法或外设业务；集成入口是 `Rehab/platform/runtime.hpp`。

## 硬件证据与实际限制

本地依据：`ProPrj_无轨迹电机控制板V1.2_2026-09-29.epro`，SHA256 `5df10e1a3c5108de99a29808f08181f27bd0d6946bbc46a2e95c9f716e34dd74`；同时核对了 `pcb-v12-mcu-rest-20260923/geometry_final.json`。这些是注明日期的快照，不代表独立完成了在线最新工程审查。在线审查结果由项目集成记录提供。

- R38/R39 标注 0.5 mΩ；板级电流采样电阻使用 500 µΩ。B 相为 PC0/ADC2 注入通道10，C 相为 PC1/ADC3 注入通道11。DRV8301 放大倍数由原生驱动根据量程选择，不能宣称固定20倍。实物电阻容差、采样极性和量程仍待上板验证。
- R50=18 kΩ、R51=1 kΩ，母线分压比例19；MCU X2=8 MHz。25 MHz 的 X1 属于 CH394Q，不是 MCU HSE。
- R25/R66 均只标注 NTC 10 kΩ；上臂接 AVCC3V3，下臂为3.3 kΩ到 AGND，节点并联2.2 µF。没有具体料号、B值或R/T曲线。本目录保留原生温度多项式作为不可用于真实使能的临时值；平台必须在温度配置确认前阻止使能。
- STOP 光耦导通时 PA7低，断开时由R18=1 kΩ拉高。外接常闭回路应断开有效，但常闭/常开语义尚需确认。PA7上拉输入只是软件安全输入；未发现其直接断开 EN_GATE。
- **制动硬件阻断项**：快照中 U12(UCC27211) 的 HS 引脚4=`G_HS`，只接自举电容C80；功率半桥中点为`$5N34`，连接Q19/Q20源极、Q15/Q16漏极、U14-1，两者没有同网或NetTie记录。必须先复核/修正硬件并验收，不能通过固件PWM极性修复。外接制动电阻阻值、功率和母线吸收能力也未确认。`DEFAULT_BRAKE_RESISTANCE=2.0` 是兼容原生结构的占位值，不能自动解除平台使能阻断。

## 资源分配

| 资源 | 分配与约束 |
|---|---|
| MCU/时钟 | STM32F405RGT6；HSE 8 MHz，SYSCLK168 MHz，APB1 42 MHz，APB2 84 MHz |
| TIM1 | M0三相PWM；PA8/9/10、PB13/14/15；原周期3500、RCR2、死区20不变 |
| TIM8 | 仅保留原中心对齐时间基准、TRGO与更新中断；无GPIO复用，CCER=0、MOE=0；不创建第二轴 |
| TIM3 | M0 AB编码器 PB4/PB5；Z为PC9，索引EXTI按原生驱动按需订阅 |
| TIM2 | 原制动时序PB10/PB11；初始浮空状态，真实制动受上述硬件阻断约束 |
| TIM5 | 可选原生PWM输入，只允许PA2/PA3，与USART2互斥；无第二轴引脚映射 |
| TIM13/TIM14 | 原生控制时间戳/HAL tick；保留原生配置 |
| ADC1 injected | PA6/channel6 母线电压，TIM1 TRGO |
| ADC1 regular | 仅PA5/channel5、PA6/channel6、PC5/channel15；3元素DMA缓冲，不扫描PC2或PA4 |
| ADC2/ADC3 | injected通道10/11负责B/C相；regular也改为10/11，仅保留TIM8调度事件，丢弃其值 |
| ADC GPIO | MSP只配置PC0、PC1、PC5、PA5、PA6为模拟输入 |
| SPI3 | PC10/11/12；DRV8301 CS=PC13；CH394Q CS=PC2；共享一个arbiter |
| SPI DMA | DMA1 Stream7/channel0 TX，高；Stream0/channel0 RX，中；IRQ分别3/4，保留先TX后RX及高于控制任务约束 |
| ADC DMA | DMA2 Stream0/channel0，循环半字，低优先级；NVIC IRQ禁用，缓冲6字节，位于可DMA访问普通SRAM |
| USART2 | PA2/PA3，逻辑UART_A；RX DMA1 Stream5/channel4，TX Stream6/channel4；IRQ10；UART_B/C、UART4不支持 |
| USB FS | PA11/PA12，保留原生USB；OTG_HS仅用作软件触发控制IRQ |
| CAN1 | PB8/PB9，原生CAN收发器；TX/RX0/RX1/SCE IRQ9 |
| STOP | PA7输入上拉；平台独立注册EXTI，板级不占用订阅 |
| HX711 | PC6/PC7/PB1，模块未启用时普通输入，无定时器/EXTI/DMA占用 |
| CH394Q | PC2启动静态高、PA4 RESET静态低、PC8输入；业务驱动接管后才初始化 |
| GPIO7 | PA15保留为用户接口；USART2 GPIO3/4、编码器GPIO9/10/11、CAN GPIO15/16以外均不提供通用引脚映射 |
| 中断优先级 | TIM8更新0；EXTI/TIM5 1；SPI DMA TX3/RX4；ControlLoop5；USB FS6；CAN9；UART/DMA10 |
| 线程 | 本目录不增加线程；一个原生axis线程2048字节栈；通信/平台任务由各所属模块创建 |
| Flash | 原链接分区：代码768 KiB、NVM256 KiB；不额外划分应用配置Flash；扩展参数持久化由项目策略决定 |
| RAM | 普通SRAM128 KiB、CCMRAM64 KiB；FreeRTOS堆配置64 KiB，链接保留最小C堆0x3C00、主栈0x800；实际镜像占用/任务栈水位须集成构建及实测报告 |

`TIM8_UP_TIM13_IRQHandler` 的漏更新、ADC EOC/JEOC齐备、控制deadline三个原生检查均保留。第二次ADC调度等待以DWT限制为10500 CPU周期（正常时钟下62.5 µs），同时有有限循环次数兜底；超时disarm并上报`ERROR_BAD_TIMING`。该上限不是实测WCET，必须上板测量总控制周期和中断抢占。

## 集成接口

- `bool rehab_board_start_general_adc()`：调用时ADC板级初始化必须已完成；成功启动三通道扫描才返回true。集成层须将false转为启动故障。
- `bool rehab_board_adc_channel_valid(uint32_t channel)`：只接受5、6、15。
- `float rehab_board_adc_voltage(uint32_t channel)`：按真实通道号映射到缓冲槽，返回伏特；未启动或非法通道返回-1。不承诺多通道原子快照；上层归一化函数必须自行除以ADC参考电压。
- `rehab::platform::early_init()`：紧接安全GPIO初始化执行。`rehab::platform::fast_guard()`：控制ISR入口调用。平台负责硬件未确认项、STOP锁存与arm准入，不能只依赖界面开关。
- SPI完成回调直接调用`spi3_arbiter.on_complete()`；错误和abort完成调用`on_error()`。没有第二个独立SPI初始化器，也不重复调用平台SPI回调。

本目录没有AXIS1对象、TIM4编码器、UART4初始化或I2C ISR。构建系统必须只选择本目录的板级文件，并选择单轴schema和独立配置版本。不得混编`Board/v3`对象。

## 验证状态

已进行源文件静态检查：无第二轴对象索引、无第二轴引脚初始化；ADC映射、TIM8输出隔离和固定hook均可追溯到本目录代码。子任务未运行ARM构建、下载或驱动硬件；集成构建由主代理执行。采样时序、电流符号、PWM死区、编码器、温度曲线、STOP极性、SPI错误恢复及制动再生均列为实机待验收。
