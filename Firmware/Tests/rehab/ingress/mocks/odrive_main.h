#pragma once
// Declaration/data carriers only. Admission, dispatch and runtime logic come
// from real production sources, never from these mock methods.
#include "../../driver_mocks/stm32_system.h"
#include "stm32_gpio.hpp"
#include "autogen/interfaces.hpp"
#include "Rehab/native_access.hpp"
#include <array>
#include <cstdint>
#include <optional>

#define AXIS_COUNT 1
#define GPIO_COUNT 17
#define TIM_APB1_CLOCK_HZ 84000000UL
struct RuntimeCritical : CriticalSectionContext {};
#define CRITICAL_SECTION() if (RuntimeCritical lock{}; true)
using StackType_t = uint32_t;
using TickType_t = uint32_t;
using osThreadId = void*;
using osMessageQId = void*;
constexpr int osPriorityAboveNormal = 1, osPriorityBelowNormal = -1;
struct MockThreadDefinition { void (*function)(void*); };
#define osThreadDef(name, function, priority, instances, stack) MockThreadDefinition os_thread_##name{function}
#define osThread(name) (&os_thread_##name)
osThreadId osThreadCreate(const MockThreadDefinition*, void*);
uint32_t xTaskGetTickCount();
uint32_t uxTaskGetStackHighWaterMark(osThreadId);
void vTaskDelayUntil(uint32_t*, uint32_t);
int osMessagePut(osMessageQId, uint32_t, uint32_t);
void NVIC_SystemReset();
struct MockRng { uint32_t SR = 1, DR = 0x12345678, CR = 0; };
extern MockRng mock_rng;
#define RNG (&mock_rng)
#define RNG_SR_DRDY 1u
#define RNG_SR_CECS 2u
#define RNG_SR_SECS 4u
#define RNG_CR_RNGEN 1u
#define __HAL_RCC_RNG_CLK_ENABLE() ((void)0)
template<typename T> struct MockOutputPort {
    std::optional<T> value = T{};
    std::optional<T> any() const { return value; }
    std::optional<T> present() const { return value; }
    MockOutputPort& operator=(T next) { value = next; return *this; }
    void reset() { value.reset(); }
};
struct Controller : ODriveIntf::ControllerIntf {
    struct Config {
        struct Anticogging { bool calib_anticogging=false, anticogging_enabled=false, pre_calibrated=false; } anticogging;
        ControlMode control_mode=CONTROL_MODE_POSITION_CONTROL;
        InputMode input_mode=INPUT_MODE_PASSTHROUGH;
        bool enable_torque_mode_vel_limit=false;
        uint32_t load_encoder_axis=0;
        float vel_limit=10, inertia=0, pos_gain=1, vel_gain=1, vel_integrator_gain=0;
    } config_;
    bool anticogging_valid_=false, trajectory_done_=false;
    uint32_t error_=0;
    float input_pos_=0, input_torque_=0, input_vel_=0, torque_setpoint_=0;
    unsigned input_updates=0, calibrations=0, mode_updates=0;
    MockOutputPort<float> torque_output_, pos_estimate_linear_src_, vel_estimate_src_;
    void input_pos_updated() { ++input_updates; }
    void set_input_pos_and_steps(float pos) { input_pos_=pos; }
    bool control_mode_updated() { ++mode_updates; return true; }
    void move_incremental(float displacement, bool) override { input_pos_+=displacement; }
    void start_anticogging_calibration() override { ++calibrations; config_.anticogging.calib_anticogging=true; }
    float remove_anticogging_bias() override { return 0; }
    float get_anticogging_value(uint32_t index) override { return float(index); }
};
struct Motor {
    enum MotorType { MOTOR_TYPE_HIGH_CURRENT=0, MOTOR_TYPE_GIMBAL=2, MOTOR_TYPE_ACIM=3 };
    struct Config { MotorType motor_type=MOTOR_TYPE_HIGH_CURRENT; float current_lim=10, torque_constant=1, torque_lim=10; } config_;
    struct CurrentControl { float Iq_measured_=0; std::optional<float2D> Idq_setpoint_{}; } current_control_;
    uint64_t error_=0;
    bool is_armed_=false, is_calibrated_=true;
    unsigned disarm_calls=0;
    bool disarm() { bool old=is_armed_; is_armed_=false; ++disarm_calls; return old; }
};
struct Encoder : ODriveIntf::EncoderIntf {
    struct Config { Mode mode=MODE_INCREMENTAL; int direction=1; uint32_t cpr=8192; } config_;
    bool is_ready_=true;
    uint32_t error_=0;
    int32_t shadow_count_=0, count_in_cpr_=0;
    unsigned count_writes=0;
    MockOutputPort<float> pos_estimate_, vel_estimate_;
    void set_linear_count(int32_t count) override { shadow_count_=count; ++count_writes; }
};
extern Motor motors[1];
extern Controller controllers[1];
extern Encoder encoders[1];
struct Axis : ODriveIntf::AxisIntf {
    struct Config {
        struct Can { uint32_t node_id=7; bool is_extended=false; } can;
        bool enable_step_dir=false, enable_sensorless_mode=false;
        bool startup_motor_calibration=false, startup_encoder_index_search=false;
        bool startup_encoder_offset_calibration=false, startup_homing=false, startup_closed_loop_control=false;
    } config_;
    Motor& motor_=motors[0];
    Encoder& encoder_=encoders[0];
    Controller& controller_=controllers[0];
    struct Sensorless { uint32_t error_=0; float pll_pos_=0; MockOutputPort<float> vel_estimate_; } sensorless_estimator_;
    struct Trajectory { struct Config { float vel_limit=10, accel_limit=10, decel_limit=10; } config_; } trap_traj_;
    std::array<AxisState,10> task_chain_{AXIS_STATE_IDLE};
    AxisState& current_state_=task_chain_[0];
    AxisState requested_state_=AXIS_STATE_UNDEFINED;
    uint32_t error_=0;
    int64_t steps_=0;
    bool step_dir_active_=false;
    Stm32Gpio step_gpio_{GPIOB,GPIO_PIN_1}, dir_gpio_{GPIOB,GPIO_PIN_2};
    unsigned watchdog_feeds=0;
    void watchdog_feed() override { ++watchdog_feeds; }
    void step_cb();
    void set_step_dir_active(bool);
};
extern Axis axes[1];
struct PWMMapping_t { endpoint_ref_t endpoint{}; float min=-1, max=1; };
struct ODrive : ODriveIntf {
    struct Config {
        bool enable_brake_resistor=false;
        float dc_bus_undervoltage_trip_level=0, dc_bus_overvoltage_trip_level=0;
        StreamProtocolType usb_cdc_protocol=STREAM_PROTOCOL_TYPE_REHAB;
        PWMMapping_t pwm_mappings[4]{}, analog_mappings[GPIO_COUNT]{};
    } config_;
    bool fault=false, misconfigured_=false;
    unsigned clears=0, saves=0, erases=0, reboots=0, dfu=0, tests=0;
    uint32_t last_adc=0, last_dma=0;
    int32_t last_irq=0;
    const int hw_version_major_=3, hw_version_minor_=12, hw_version_variant_=0;
    const int fw_version_major_=0, fw_version_minor_=5, fw_version_revision_=6;
    bool any_error() const { return fault; }
    void clear_errors() override;
    int32_t test_function(int32_t delta) override { ++tests; return delta; }
    float get_adc_voltage(uint32_t gpio) override { last_adc=gpio; return float(gpio)/10; }
    bool save_configuration() override { ++saves; return true; }
    void erase_configuration() override { ++erases; }
    void reboot() override { ++reboots; }
    void enter_dfu_mode() override { ++dfu; }
    uint32_t get_interrupt_status(int32_t irq) override { last_irq=irq; return uint32_t(irq); }
    uint32_t get_dma_status(uint8_t stream) override { last_dma=stream; return stream; }
    uint32_t get_gpio_states() override { return 0x123; }
    uint64_t get_drv_fault() override { return 0; }
    struct Scope : OscilloscopeIntf {
        uint32_t last_index=0;
        float get_val(uint32_t index) override { last_index=index; return float(index); }
    } oscilloscope_;
};
extern ODrive odrv;
extern const char serial_number_str[];
struct MockThermistor { struct Config { float temp_limit_lower=0, temp_limit_upper=0; } config_; };
extern MockThermistor fet_thermistors[1];
struct MockSpiArbiter {
    struct Diagnostics { uint32_t timeouts=0, errors=0, rejected=0, highwater=0; bool quarantined=false; };
    void service(uint32_t) {}
    Diagnostics diagnostics() const { return {}; }
};
extern MockSpiArbiter ext_spi_arbiter;
extern float vbus_voltage, ibus_;
float rehab_board_adc_voltage(uint32_t);
Stm32Gpio get_gpio(size_t);
float get_adc_voltage(Stm32Gpio);
