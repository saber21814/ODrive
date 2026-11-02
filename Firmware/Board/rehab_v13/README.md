# V1.3 single-axis board adaptation

This independent board target derives from `Board/rehab_v12`; do not link its objects with another board directory. Build as `rehab-v1.3` with configuration version `0x1301`. `REHAB_V12` remains the existing shared Rehab hook ABI; `REHAB_V13` selects this hardware revision. The original DRV8301, FOC, controller and encoder algorithms are reused without copies or rewrites.

Hardware evidence: the live V1.3 EasyEDA project reviewed on 2026-10-29. Critical schematic and PCB component nets agreed. This is not a completed PCB DRC or physical-board acceptance; the DRC API did not complete.

| Resource | V1.3 mapping |
|---|---|
| Clock | STM32F405RGT6, HSE 8 MHz, SYSCLK 168 MHz, PCLK1 42 MHz, PCLK2 84 MHz |
| M0 PWM and encoder | Existing TIM1 three-phase PWM, TIM3 AB and PC9 Z; TIM8 scheduling only, no outputs |
| Phase current | PC0 / ADC2 injected10, PC1 / ADC3 injected11; R38/R39 0.5 mOhm |
| Bus voltage | PA1 / ADC1 injected1; R17 18 kOhm, R21 1 kOhm, divider ratio19 |
| Regular ADC | `{1,8,15}`: PA1 bus, PB0 brake temperature, PC5 M0 temperature; DMA2 Stream0 Channel0 |
| ADC GPIO | Only PC0/1/5, PA1, PB0. ADC reinitialization never reclaims PA5/6/7 |
| CH394Q | SPI1 PA5/6/7, 8-bit mode0, 84 MHz /32 =2.625 MHz; CS PC2, RESET PA4, INT PC8 |
| SPI1 DMA | RX DMA2 Stream2 Channel3, TX DMA2 Stream3 Channel3; IRQ RX4 / TX3 |
| DRV and optional SPI encoder | SPI3 PC10/11/12, DRV CS PC13; existing SPI3 settings |
| SPI3 DMA | RX DMA1 Stream0 Channel0, TX DMA1 Stream7 Channel0; IRQ RX4 / TX3 |
| STOP | PC4, active low, external 10 kOhm pull-down, GPIO_NOPULL, EXTI4 both edges subscribed by runtime |
| GATE | PB12 is the STOP AND-gate input; EN_GATE is its output. Latch low before selecting output mode |
| DC_CAL | PB7, latch low before push-pull output, held low during normal operation |
| Thermistors | HNTC0805-103F3950FB, 10 kOhm at25C, B25/50=3950 K; NTC upper leg, 3.3 kOhm lower leg |
| Others | Existing USART2, USB FS, CAN1 PB8 RX / PB9 TX, HX711 PC6/PC7/PB1, AUX PB10/PB11 |

Two independent `Stm32SpiArbiter` instances own their queues, DMA buffers, diagnostics and recovery. `ext_spi_arbiter` is SPI1; DRV and encoder explicitly use SPI3. HAL completion/error/abort callbacks dispatch by the actual SPI handle. SPI1 never uses the ADC's DMA2 Stream0.

`Rehab/drivers/v13_gate_driver.hpp` composes the original DRV8301 driver. On physical STOP, runtime disarms motor PWM first, then calls `rehab_board_physical_stop` to invalidate readiness, advance the generation and lower GATE. The ISR path performs no SPI or waits. Repeated low-level guards are idempotent; each edge notification advances the generation, including a pulse already recovered by ISR entry.

Task-context initialization permits native startup UNDEFINED or normal IDLE only, with PWM disabled and STOP high. It uses the original public `config(10) -> config(80) -> config(requested_gain)` API to invalidate the private ready cache, followed by native `init()` register programming/readback. STOP, generation and disarmed state are checked before raising GATE and after initialization. Ordinary idle does not reset DRV repeatedly and retains current-sense bias. Restoring STOP does not clear platform faults or enable motion; an explicit enable is still required after fault clearing.

The V1.3 netlist connects U12 HS to the real half-bridge midpoint, resolving the old disconnected-HS netlist defect. The new brake selector also has an autonomous hardware path: STOP=DELAY=1 selects MCU AUX_H/AUX_L; unequal levels disable outputs; both low select 555-driven HI and LI=0. Disabling software braking therefore does not disable the hardware branch when STOP is low.

Real motor enable remains off by default. Physical temperature thresholds, motor parameters, brake resistor, regeneration and timing acceptance remain separate prerequisites. Host tests validate software logic only. GATE/PWM shutdown, DRV register restoration, ADC synchronization, both SPI waveforms, brake switching and regeneration heating need physical validation. This change does not flash firmware, run a motor or edit the circuit.
