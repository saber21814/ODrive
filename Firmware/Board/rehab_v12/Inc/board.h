/*
* @brief Rehabilitation V1.2 single-axis board adaptation of ODrive v0.5.6
*/

#ifndef __BOARD_CONFIG_H
#define __BOARD_CONFIG_H

#include <stdbool.h>

// STM specific includes
#include <stm32f4xx_hal.h>
#include <gpio.h>
#include <spi.h>
#include <tim.h>
#include <can.h>
#include <usb_device.h>
#include <main.h>
#include "cmsis_os.h"

#include <arm_math.h>

#include <Drivers/STM32/stm32_system.h>

#ifndef REHAB_V12
#define REHAB_V12 1
#endif
#define SHUNT_RESISTANCE (500e-6f)

#define AXIS_COUNT (1)

// Total count of GPIOs, including encoder pins, CAN pins and a dummy GPIO0.
// Unconnected/reserved pins keep their native numbers but map to null GPIOs.
#define GPIO_COUNT  (17)

#define CAN_FREQ (2000000UL)

#define DEFAULT_BRAKE_RESISTANCE (2.0f) // unverified placeholder; platform motion interlock remains closed

#define DEFAULT_ERROR_PIN 0
#define DEFAULT_MIN_DC_VOLTAGE 8.0f

#define DEFAULT_GPIO_MODES \
    ODriveIntf::GPIO_MODE_DIGITAL, \
    ODriveIntf::GPIO_MODE_DIGITAL, \
    ODriveIntf::GPIO_MODE_DIGITAL, \
    ODriveIntf::GPIO_MODE_UART_A, \
    ODriveIntf::GPIO_MODE_UART_A, \
    ODriveIntf::GPIO_MODE_DIGITAL, \
    ODriveIntf::GPIO_MODE_DIGITAL, \
    ODriveIntf::GPIO_MODE_DIGITAL, \
    ODriveIntf::GPIO_MODE_DIGITAL, \
    ODriveIntf::GPIO_MODE_ENC0, \
    ODriveIntf::GPIO_MODE_ENC0, \
    ODriveIntf::GPIO_MODE_DIGITAL_PULL_DOWN, \
    ODriveIntf::GPIO_MODE_DIGITAL, \
    ODriveIntf::GPIO_MODE_DIGITAL, \
    ODriveIntf::GPIO_MODE_DIGITAL, \
    ODriveIntf::GPIO_MODE_CAN_A, \
    ODriveIntf::GPIO_MODE_CAN_A, 

#define TIM_TIME_BASE TIM14

// Run control loop at the same frequency as the current measurements.
#define CONTROL_TIMER_PERIOD_TICKS  (2 * TIM_1_8_PERIOD_CLOCKS * (TIM_1_8_RCR + 1))

#define TIM1_INIT_COUNT (TIM_1_8_PERIOD_CLOCKS / 2 - 1 * 128) // TODO: explain why this offset

// Preserve the native TIM1/TIM8 phase and the original update deadline bound.
#define MAX_CONTROL_LOOP_UPDATE_TO_CURRENT_UPDATE_DELTA (TIM_1_8_PERIOD_CLOCKS / 2 + 1 * 128)

#ifdef __cplusplus
#include <Drivers/DRV8301/drv8301.hpp>
#include <Drivers/STM32/stm32_gpio.hpp>
#include <Drivers/STM32/stm32_spi_arbiter.hpp>
#include <MotorControl/pwm_input.hpp>
#include <MotorControl/thermistor.hpp>

using TGateDriver = Drv8301;
using TOpAmp = Drv8301;

#include <MotorControl/motor.hpp>
#include <MotorControl/encoder.hpp>

extern std::array<Axis, AXIS_COUNT> axes;
extern Motor motors[AXIS_COUNT];
extern OnboardThermistorCurrentLimiter fet_thermistors[AXIS_COUNT];
extern Encoder encoders[AXIS_COUNT];
extern Stm32Gpio gpios[GPIO_COUNT];

struct GpioFunction { int mode = 0; uint8_t alternate_function = 0xff; };
extern std::array<GpioFunction, 3> alternate_functions[GPIO_COUNT];

extern USBD_HandleTypeDef& usb_dev_handle;

extern Stm32SpiArbiter& ext_spi_arbiter;

extern UART_HandleTypeDef* uart_a;
extern UART_HandleTypeDef* uart_b;
extern UART_HandleTypeDef* uart_c;

extern PwmInput pwm0_input;
#endif

// Period in [s]
#define CURRENT_MEAS_PERIOD ( (float)2*TIM_1_8_PERIOD_CLOCKS*(TIM_1_8_RCR+1) / (float)TIM_1_8_CLOCK_HZ )
static const float current_meas_period = CURRENT_MEAS_PERIOD;

// Frequency in [Hz]
#define CURRENT_MEAS_HZ ( (float)(TIM_1_8_CLOCK_HZ) / (float)(2*TIM_1_8_PERIOD_CLOCKS*(TIM_1_8_RCR+1)) )
static const int current_meas_hz = CURRENT_MEAS_HZ;

#define VBUS_S_DIVIDER_RATIO 19.0f // R50=18k, R51=1k

// Linear range of the DRV8301 opamp output: 0.3V...5.7V. We set the upper limit
// to 3.0V so that it's symmetric around the center point of 1.65V.
#define CURRENT_SENSE_MIN_VOLT  0.3f
#define CURRENT_SENSE_MAX_VOLT  3.0f

// This board has no board-specific user configurations
static inline bool board_read_config() { return true; }
static inline bool board_write_config() { return true; }
static inline void board_clear_config() { }
static inline bool board_apply_config() { return true; }

void system_init();
bool board_init();
bool rehab_board_start_general_adc();
bool rehab_board_adc_channel_valid(uint32_t channel);
float rehab_board_adc_voltage(uint32_t channel);
void start_timers();

#endif // __BOARD_CONFIG_H
