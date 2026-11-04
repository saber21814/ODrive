#pragma once
// Minimal ODrive/RTOS carrier types for the REAL runtime.cpp integration tests.
// Motor dynamics, vendor HAL and native FOC algorithms are not simulated here.
#include "../driver_mocks/stm32_system.h"
#include "stm32_gpio.hpp"
#include "Rehab/native_access.hpp"
#include <array>
#include <cstdint>
#include <optional>
#if defined(REHAB_V13)
#define STOP_GPIO_Port GPIOC
#define STOP_Pin GPIO_PIN_4
#endif

struct RuntimeCritical : CriticalSectionContext {};
#define CRITICAL_SECTION() if (RuntimeCritical runtime_critical_{}; true)

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
struct Controller {
    enum ControlMode { CONTROL_MODE_TORQUE_CONTROL = 1, CONTROL_MODE_VELOCITY_CONTROL = 2, CONTROL_MODE_POSITION_CONTROL = 3 };
    enum InputMode { INPUT_MODE_PASSTHROUGH = 1, INPUT_MODE_VEL_RAMP = 2, INPUT_MODE_TORQUE_RAMP = 6 };
    struct Config {
        struct Anticogging { bool calib_anticogging = false, anticogging_enabled = false, pre_calibrated = false; } anticogging;
        ControlMode control_mode = CONTROL_MODE_POSITION_CONTROL;
        InputMode input_mode = INPUT_MODE_PASSTHROUGH;
        bool enable_torque_mode_vel_limit = false, enable_overspeed_error=true;
        uint32_t load_encoder_axis = 0;
        Controller* parent = nullptr;
        float vel_limit=2, vel_limit_tolerance=1.2f, vel_ramp_rate=1, vel_integrator_limit=0;
    } config_;
    using Config_t=Config;
    uint32_t error_=0, input_pos_updates=0;
    float input_pos_=0, vel_integrator_torque_=0;
    bool apply_config() { config_.parent=this; return true; }
    void reset() { vel_integrator_torque_=torque_setpoint_=0; }
    void input_pos_updated() { ++input_pos_updates; }
    bool anticogging_valid_ = false;
    float input_torque_ = 0, input_vel_ = 0, torque_setpoint_ = 0;
    MockOutputPort<float> torque_output_;
};
struct Motor {
    enum MotorType { MOTOR_TYPE_HIGH_CURRENT = 0, MOTOR_TYPE_GIMBAL = 2, MOTOR_TYPE_ACIM = 3 };
    struct Config {
        MotorType motor_type = MOTOR_TYPE_HIGH_CURRENT; float current_lim = 10, torque_constant = 1;
        Motor* parent=nullptr;
        int pole_pairs=7;
        float calibration_current=10, resistance_calib_max_voltage=2, requested_current_range=60;
        float current_lim_margin=8, torque_lim=0, I_bus_hard_min=0, I_bus_hard_max=0;
        float phase_resistance=0, phase_inductance=0, dc_calib_tau=0.2f;
        bool pre_calibrated=false;
    } config_;
    using Config_t=Config;
    struct CurrentControl { float Iq_measured_ = 0, Id_measured_=0; } current_control_;
    struct PhaseCurrent { float phA=0, phB=0, phC=0; } DC_calib_;
    float dc_calib_running_since_=0,max_dc_calib_=0,phase_current_rev_gain_=0;
    uint64_t error_=0;
    struct Thermistor { struct Config { bool enabled=false; } config_; } motor_thermistor_;
    bool apply_config() { config_.parent=this;is_calibrated_=config_.pre_calibrated; return true; }
    bool is_armed_ = false, is_calibrated_ = true;
    unsigned disarm_calls = 0;
    bool disarm() { const bool old = is_armed_; is_armed_ = false; ++disarm_calls; return old; }
};
struct Encoder {
    enum Mode { MODE_INCREMENTAL = 0, MODE_HALL = 1, MODE_FLAG_ABS = 0x100 };
    struct Config {
        Mode mode = MODE_INCREMENTAL; int direction = 1; uint32_t cpr = 8192;
        Encoder* parent=nullptr;
        bool use_index=false, pre_calibrated=false;
    } config_;
    using Config_t=Config;
    uint32_t error_=0;
    int32_t shadow_count_=0;
    bool apply_config(Motor::MotorType) { config_.parent=this;return true; }
    bool is_ready_ = true;
    MockOutputPort<float> pos_estimate_, vel_estimate_;
};
extern Motor motors[1];
extern Controller controllers[1];
extern Encoder encoders[1];
struct Axis {
    enum AxisState : uint32_t { AXIS_STATE_UNDEFINED = 0, AXIS_STATE_IDLE = 1,
        AXIS_STATE_STARTUP_SEQUENCE = 2, AXIS_STATE_FULL_CALIBRATION_SEQUENCE = 3,
        AXIS_STATE_MOTOR_CALIBRATION = 4, AXIS_STATE_ENCODER_OFFSET_CALIBRATION=7, AXIS_STATE_CLOSED_LOOP_CONTROL = 8,
        AXIS_STATE_LOCKIN_SPIN=9 };
    struct LockinConfig_t {
        float current=10.0f, ramp_time=0.4f, ramp_distance=3.14159265358979323846f;
        float accel=20.0f, vel=40.0f, finish_distance=100.0f;
        bool finish_on_vel=false,finish_on_distance=false,finish_on_enc_idx=false;
    };
    struct Config {
        bool enable_step_dir = false, enable_sensorless_mode = false;
        bool startup_motor_calibration = false, startup_encoder_index_search = false;
        bool startup_encoder_offset_calibration = false, startup_homing = false;
        bool startup_closed_loop_control = false;
        Axis* parent=nullptr;
        bool enable_watchdog=false, step_dir_always_on=false;
        float watchdog_timeout=0;
        LockinConfig_t general_lockin;
    } config_;
    using Config_t=Config;
    struct Endstop { struct Config { bool enabled=false; } config_; } min_endstop_,max_endstop_;
    struct Brake { struct Config { uint32_t gpio_num=0; } config_; } mechanical_brake_;
    uint32_t error_=0;
    Motor& motor_ = motors[0];
    Encoder& encoder_ = encoders[0];
    Controller& controller_ = controllers[0];
    std::array<AxisState,10> task_chain_{AXIS_STATE_IDLE};
    AxisState& current_state_ = task_chain_[0];
    AxisState requested_state_ = AXIS_STATE_UNDEFINED;
    bool step_dir_active_ = false;
    unsigned watchdog_feeds = 0;
    void watchdog_feed() { ++watchdog_feeds; }
};
extern Axis axes[1];
struct ODrive {
    enum StreamProtocolType { STREAM_PROTOCOL_TYPE_REHAB = 4 };
    struct Config {
        bool enable_brake_resistor = false;
        float dc_bus_undervoltage_trip_level = 0, dc_bus_overvoltage_trip_level = 0;
        float dc_max_positive_current=0,dc_max_negative_current=0,brake_resistance=2;
        bool enable_dc_bus_overvoltage_ramp=false;
        StreamProtocolType usb_cdc_protocol = STREAM_PROTOCOL_TYPE_REHAB;
    } config_;
    bool fault = false;
    uint32_t error_=0,user_config_loaded_=0;
    bool any_error() const { return fault || error_ || axes[0].error_ || motors[0].error_ || encoders[0].error_ || controllers[0].error_; }
    void clear_errors();
};
extern ODrive odrv;
struct MockThermistor { struct Config { float temp_limit_lower = 0, temp_limit_upper = 0; bool enabled=false; } config_; };
extern MockThermistor fet_thermistors[1];
struct MockSpiArbiter {
    struct Diagnostics { uint32_t timeouts = 0, errors = 0, rejected = 0, highwater = 0; bool quarantined = false; };
    void service(uint32_t) { ++services; }
    Diagnostics diagnostics() const { return {}; }
    unsigned services = 0;
};
extern MockSpiArbiter ext_spi_arbiter;
extern float vbus_voltage;
float rehab_board_adc_voltage(uint32_t);
