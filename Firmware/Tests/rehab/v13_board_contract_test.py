"""Source-level board contracts complement production runtime/driver tests.
They explicitly cover the cold-boot STOP ordering that host HAL mocks miss.
"""
from pathlib import Path
import re

FW = Path(__file__).resolve().parents[2]
BOARD = FW / "Board" / "rehab_v13"

def read(name):
    return (BOARD / name).read_text(encoding="utf-8")

def main():
    board = read("board.cpp")
    init = board.split("bool board_init() {", 1)[1].split("void start_timers()", 1)[0]
    # On cold boot, physical STOP calls Motor::disarm, which dereferences the
    # TIM handle. Every handle must exist before that callback can execute.
    assert init.index("MX_TIM1_Init();") < init.index("rehab::platform::early_init();")
    assert init.index("MX_TIM13_Init();") < init.index("rehab::platform::early_init();")
    assert init.index("rehab::platform::early_init();") < init.index("HAL_NVIC_EnableIRQ(EXTI4_IRQn)")
    assert "drv_enable_gpio.write(true)" not in init
    # A disabled network module must not claim its dedicated SPI/DMA hardware.
    assert "#if REHAB_ENABLE_NETWORK\n    MX_SPI1_Init();\n#endif" in init
    dma = read("Src/dma.c")
    guarded = dma.split("#if REHAB_ENABLE_NETWORK", 1)[1].split("#endif", 1)[0]
    assert "HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);" in guarded
    assert "HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);" in guarded

    main_h = read("Inc/main.h")
    expected = {"VBUS_S_Pin": "GPIO_PIN_1", "VBUS_S_GPIO_Port": "GPIOA",
                "AUX_TEMP_Pin": "GPIO_PIN_0", "AUX_TEMP_GPIO_Port": "GPIOB",
                "M0_TEMP_Pin": "GPIO_PIN_5", "M0_TEMP_GPIO_Port": "GPIOC",
                "STOP_Pin": "GPIO_PIN_4", "STOP_GPIO_Port": "GPIOC",
                "DC_CAL_Pin": "GPIO_PIN_7", "DC_CAL_GPIO_Port": "GPIOB"}
    for name, value in expected.items():
        assert re.search(r"^#define\s+" + name + r"\s+" + value + r"\s*$", main_h, re.M), name
    adc = read("Src/adc.c")
    assert "sConfigInjected.InjectedChannel = ADC_CHANNEL_1;" in adc
    assert "sConfig.Channel = ADC_CHANNEL_1;" in adc
    assert "general_adc_channels[3] = {1, 8, 15}" in board
    assert "AUX_TEMP_Pin|VBUS_S_Pin" not in adc
    assert "GPIO_PIN_5" not in adc and "GPIO_PIN_6" not in adc and "GPIO_PIN_7" not in adc
    assert "HAL_GPIO_Init(VBUS_S_GPIO_Port, &GPIO_InitStruct);" in adc
    assert "HAL_GPIO_Init(AUX_TEMP_GPIO_Port, &GPIO_InitStruct);" in adc

    gpio = read("Src/gpio.c")
    dc_latch = "HAL_GPIO_WritePin(GPIOB, EN_GATE_Pin | DC_CAL_Pin | AUX_L_Pin | AUX_H_Pin, GPIO_PIN_RESET);"
    assert gpio.index(dc_latch) < gpio.index("cfg.Mode = GPIO_MODE_OUTPUT_PP;")
    stop = gpio.split("// STOP is low-active", 1)[1]
    assert stop.index("cfg.Pull = GPIO_NOPULL;") < stop.index("HAL_GPIO_Init(STOP_GPIO_Port, &cfg);")

    spi = read("Src/spi.c")
    assert "hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;" in spi
    assert "hdma_spi1_tx.Instance = DMA2_Stream3;" in spi
    assert "hdma_spi1_rx.Instance = DMA2_Stream2;" in spi
    assert "hdma_spi1_tx.Init.Channel = DMA_CHANNEL_3;" in spi
    assert "hdma_spi1_rx.Init.Channel = DMA_CHANNEL_3;" in spi
    assert "hdma_adc1.Instance = DMA2_Stream0;" in adc
    assert "Stm32SpiArbiter& ext_spi_arbiter = spi1_arbiter;" in board
    for callback in ["on_complete", "on_error"]:
        assert "if (hspi == &hspi3) { spi3_arbiter." + callback + "(); }" in board
        assert "else if (hspi == &hspi1) { spi1_arbiter." + callback + "(); }" in board
    print("v13_board_contract: cold boot, mapping, DMA isolation and callbacks passed")

if __name__ == "__main__":
    main()
