#pragma once
#include <cstdint>
inline void (*mock_gpio_bsrr_hook)(uint32_t) = nullptr;
struct MockBsrr {
    uint32_t value = 0;
    MockBsrr& operator=(uint32_t next) { value = next; if (mock_gpio_bsrr_hook) mock_gpio_bsrr_hook(next); return *this; }
    operator uint32_t() const { return value; }
};
struct GPIO_TypeDef { uint32_t IDR = 0; MockBsrr BSRR; };
extern GPIO_TypeDef mock_gpio_a, mock_gpio_b, mock_gpio_c;
#define GPIOA (&mock_gpio_a)
#define GPIOB (&mock_gpio_b)
#define GPIOC (&mock_gpio_c)
#define GPIO_PIN_1 (1u << 1)
#define GPIO_PIN_2 (1u << 2)
#define GPIO_PIN_4 (1u << 4)
#define GPIO_PIN_6 (1u << 6)
#define GPIO_PIN_7 (1u << 7)
#define GPIO_PIN_8 (1u << 8)
#define GPIO_PIN_SET 1
#define GPIO_PIN_RESET 0
#define GPIO_MODE_INPUT 0
#define GPIO_MODE_OUTPUT_PP 1
#define GPIO_NOPULL 0
#define GPIO_PULLUP 1
#define GPIO_SPEED_FREQ_LOW 0
#define __HAL_RCC_GPIOA_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_GPIOC_CLK_ENABLE() ((void)0)
inline void HAL_GPIO_WritePin(GPIO_TypeDef* port, uint16_t pin, int state) {
    if (state) port->IDR |= pin; else port->IDR &= ~pin;
}
