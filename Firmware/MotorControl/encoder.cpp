
#include "odrive_main.h"
#include <Drivers/STM32/stm32_system.h>
#include <algorithm>
#include <bitset>
#include "as5600_utils.hpp"

#if HW_VERSION_MAJOR == 3
static Encoder* as5600_owner = nullptr;

extern "C" void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef* hi2c) {
    if (hi2c == &hi2c1 && as5600_owner)
        as5600_owner->as5600_rx_complete();
}

extern "C" void HAL_I2C_ErrorCallback(I2C_HandleTypeDef* hi2c) {
    if (hi2c == &hi2c1 && as5600_owner)
        as5600_owner->as5600_i2c_error();
}
#endif

Encoder::Encoder(TIM_HandleTypeDef* timer, Stm32Gpio index_gpio,
                 Stm32Gpio hallA_gpio, Stm32Gpio hallB_gpio, Stm32Gpio hallC_gpio,
                 Stm32SpiArbiter* spi_arbiter) :
        timer_(timer), index_gpio_(index_gpio),
        hallA_gpio_(hallA_gpio), hallB_gpio_(hallB_gpio), hallC_gpio_(hallC_gpio),
        spi_arbiter_(spi_arbiter)
{
}

static void enc_index_cb_wrapper(void* ctx) {
    reinterpret_cast<Encoder*>(ctx)->enc_index_cb();
}

bool Encoder::apply_config(ODriveIntf::MotorIntf::MotorType motor_type) {
    config_.parent = this;

    if (config_.mode == MODE_I2C_ABS_AS5600) {
#if HW_VERSION_MAJOR == 3
        if (!axis_ || axis_->axis_num_ != 1 || config_.cpr != as5600::CPR ||
            config_.use_index || !std::isfinite(config_.phase_delay_compensation) ||
            config_.phase_delay_compensation < 0.0f ||
            config_.phase_delay_compensation > 0.005f) {
            return false;
        }
#else
        return false;
#endif
    }

    update_pll_gains();

    if (config_.pre_calibrated) {
        if (config_.mode == Encoder::MODE_HALL && config_.hall_polarity_calibrated)
            is_ready_ = true;
        if (config_.mode == Encoder::MODE_SINCOS)
            is_ready_ = true;
        if (motor_type == Motor::MOTOR_TYPE_ACIM)
            is_ready_ = true;
    }

    return true;
}

void Encoder::setup() {
    HAL_TIM_Encoder_Start(timer_, TIM_CHANNEL_ALL);
    set_idx_subscribe();

    mode_ = config_.mode;

    spi_task_.config = {
        .Mode = SPI_MODE_MASTER,
        .Direction = SPI_DIRECTION_2LINES,
        .DataSize = SPI_DATASIZE_16BIT,
        .CLKPolarity = (mode_ == MODE_SPI_ABS_AEAT || mode_ == MODE_SPI_ABS_MA732) ? SPI_POLARITY_HIGH : SPI_POLARITY_LOW,
        .CLKPhase = SPI_PHASE_2EDGE,
        .NSS = SPI_NSS_SOFT,
        .BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16,
        .FirstBit = SPI_FIRSTBIT_MSB,
        .TIMode = SPI_TIMODE_DISABLE,
        .CRCCalculation = SPI_CRCCALCULATION_DISABLE,
        .CRCPolynomial = 10,
    };

    if (mode_ == MODE_SPI_ABS_MA732) {
        abs_spi_dma_tx_[0] = 0x0000;
    }

    if((mode_ & MODE_FLAG_ABS) && mode_ != MODE_I2C_ABS_AS5600){
        abs_spi_cs_pin_init();
    }

    if ((mode_ & MODE_FLAG_ABS) && axis_->controller_.config_.anticogging.pre_calibrated)
        axis_->controller_.anticogging_valid_ = true;

    if (mode_ == MODE_I2C_ABS_AS5600) {
#if HW_VERSION_MAJOR == 3
        if (!axis_ || axis_->axis_num_ != 1 || i2c1_as5600_master == 0 ||
            config_.cpr != as5600::CPR || config_.use_index) {
            odrv.misconfigured_ = true;
            set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
            return;
        }
        as5600_owner = this;
        if (!as5600_configure_volatile()) {
            as5600_recovery_blocked_ = true;
            set_error(as5600_config_i2c_ok_ ? ERROR_ABS_I2C_MAGNET_ERROR :
                      ERROR_ABS_I2C_COM_FAIL);
        } else {
            as5600_recovery_blocked_ = false;
            as5600_monitor_start_tick_ = odrv.n_evt_sampling_;
        }
#else
        odrv.misconfigured_ = true;
        set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
#endif
    }
}

bool Encoder::as5600_configure_volatile() {
#if HW_VERSION_MAJOR == 3
    as5600_config_i2c_ok_ = false;
    uint8_t conf_buf[2] = {0, 0};
    if (HAL_I2C_Mem_Read(&hi2c1, 0x36 << 1, 0x07, I2C_MEMADD_SIZE_8BIT,
                         conf_buf, 2, 10) != HAL_OK) {
        return false;
    }
    uint16_t conf = (uint16_t)(((uint16_t)conf_buf[0] << 8) | conf_buf[1]);
    uint16_t desired = as5600::configure_conf(conf);
    uint8_t desired_buf[2] = {(uint8_t)(desired >> 8), (uint8_t)desired};
    if (HAL_I2C_Mem_Write(&hi2c1, 0x36 << 1, 0x07, I2C_MEMADD_SIZE_8BIT,
                          desired_buf, 2, 10) != HAL_OK) {
        return false;
    }
    uint8_t verify_buf[2] = {0, 0};
    if (HAL_I2C_Mem_Read(&hi2c1, 0x36 << 1, 0x07, I2C_MEMADD_SIZE_8BIT,
                         verify_buf, 2, 10) != HAL_OK) {
        return false;
    }
    uint16_t verify = (uint16_t)(((uint16_t)verify_buf[0] << 8) | verify_buf[1]);
    uint8_t status = 0;
    if (verify == desired &&
        HAL_I2C_Mem_Read(&hi2c1, 0x36 << 1, 0x0b, I2C_MEMADD_SIZE_8BIT,
                         &status, 1, 10) == HAL_OK) {
        as5600_status_buf_[0] = status;
        as5600_status_ = status;
        as5600_last_status_tick_ = odrv.n_evt_sampling_;
        as5600_have_status_ = true;
        as5600_config_i2c_ok_ = true;
        return as5600::magnet_detected(status);
    }
    return false;
#else
    return false;
#endif
}

bool Encoder::as5600_start_read(uint8_t reg, uint8_t* buf, uint16_t len, uint8_t request) {
#if HW_VERSION_MAJOR == 3
    const bool hal_ready = hi2c1.State == HAL_I2C_STATE_READY;
    const bool bus_busy = __HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) != RESET;
    if (!as5600::transaction_can_start(as5600_i2c_pending_,
                                       as5600_recovery_requested_,
                                       as5600_recovery_blocked_,
                                       hal_ready, bus_busy)) {
        if (!as5600_i2c_pending_ && !as5600_recovery_requested_ &&
            !as5600_recovery_blocked_ && (!hal_ready || bus_busy))
            as5600_busy_skip_seq_++;
        return false;
    }
    // HAL_I2C_Mem_Read_IT() contains a blocking BUSY wait before it starts the
    // interrupt transaction. This function runs in the highest-priority
    // sampling ISR, so reject a busy peripheral before entering HAL.
    as5600_i2c_request_ = request;
    as5600_i2c_pending_ = 1;
    if (HAL_I2C_Mem_Read_IT(&hi2c1, 0x36 << 1, reg, I2C_MEMADD_SIZE_8BIT,
                            buf, len) != HAL_OK) {
        as5600_i2c_pending_ = 0;
        as5600_note_i2c_failure();
        return false;
    }
    return true;
#else
    (void)reg;
    (void)buf;
    (void)len;
    (void)request;
    return false;
#endif
}

void Encoder::as5600_rx_complete() {
#if HW_VERSION_MAJOR == 3
    // The ISR is deliberately limited to publishing the completed transaction.
    if (as5600_i2c_request_ == 1) {
        as5600_last_sample_tick_ = odrv.n_evt_sampling_;
        __DMB();
        as5600_sample_seq_++;
    }
    if (as5600_i2c_request_ == 2) {
        as5600_last_status_tick_ = odrv.n_evt_sampling_;
        as5600_have_status_ = true;
        __DMB();
        as5600_status_seq_++;
    }
    as5600_complete_seq_++;
    as5600_irq_consecutive_errors_ =
        as5600::update_failure_count(as5600_irq_consecutive_errors_, true);
    as5600_i2c_pending_ = 0;
#endif
}

void Encoder::as5600_note_i2c_failure() {
#if HW_VERSION_MAJOR == 3
    as5600_complete_seq_++;
    as5600_failed_seq_++;
    as5600_error_seq_++;
    as5600_irq_consecutive_errors_ =
        as5600::update_failure_count(as5600_irq_consecutive_errors_, false);
    if (as5600_irq_consecutive_errors_ >= as5600::MAX_CONSECUTIVE_FAILURES)
        as5600_recovery_requested_ = true;
#endif
}

void Encoder::as5600_i2c_error() {
#if HW_VERSION_MAJOR == 3
    as5600_i2c_pending_ = 0;
    as5600_note_i2c_failure();
#endif
}

void Encoder::as5600_service_idle() {
#if HW_VERSION_MAJOR == 3
    if (!as5600::recovery_can_run(as5600_recovery_requested_, axis_ != nullptr,
                                  axis_ ? axis_->motor_.is_armed_ : false))
        return;

    // This service is called from the low-priority Axis IDLE task. Never move
    // HAL deinitialization or GPIO clock recovery back into Encoder::update().
    as5600_recovery_blocked_ = true;
    as5600_i2c_pending_ = 0;
    HAL_I2C_DeInit(&hi2c1);

    GPIO_InitTypeDef gpio = {};
    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6 | GPIO_PIN_7, GPIO_PIN_SET);
    delay_us(2);
    for (uint8_t pulse = 0; pulse < as5600::RECOVERY_CLOCKS; ++pulse) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
        delay_us(2);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
        delay_us(2);
    }
    // Generate a STOP while SCL is high.
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET);
    delay_us(2);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
    delay_us(2);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);
    delay_us(2);

    gpio.Mode = GPIO_MODE_AF_OD;
    gpio.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &gpio);
    MX_I2C1_AS5600_Init();

    as5600_irq_consecutive_errors_ = 0;
    as5600_consecutive_errors_ = 0;
    as5600_resume_after_recovery_ = as5600_have_sample_;
    as5600_monitor_start_tick_ = odrv.n_evt_sampling_;
    if (!as5600_configure_volatile()) {
        as5600_recovery_blocked_ = true;
        set_error(as5600_config_i2c_ok_ ? ERROR_ABS_I2C_MAGNET_ERROR :
                  ERROR_ABS_I2C_COM_FAIL);
    } else {
        as5600_recovery_blocked_ = false;
    }
    as5600_recovery_requested_ = false;
#endif
}

bool Encoder::as5600_closed_loop_ready() const {
    if (mode_ != MODE_I2C_ABS_AS5600)
        return true;
    return is_ready_ && as5600_have_sample_ &&
           as5600_have_status_ &&
           !as5600::status_is_stale(odrv.n_evt_sampling_,
                                    as5600_last_status_tick_) &&
           config_.as5600_calibration_version == AS5600_CALIBRATION_VERSION &&
           (config_.direction == 1 || config_.direction == -1) &&
           sample_age_ < 0.001f && as5600::magnet_detected(as5600_status_) &&
           error_ == ERROR_NONE && !as5600_recovery_requested_ &&
           !as5600_recovery_blocked_;
}

void Encoder::set_error(Error error) {
    vel_estimate_valid_ = false;
    pos_estimate_valid_ = false;
    error_ |= error;
    axis_->error_ |= Axis::ERROR_ENCODER_FAILED;
}

bool Encoder::do_checks(){
    return error_ == ERROR_NONE;
}

//--------------------
// Hardware Dependent
//--------------------

// Triggered when an encoder passes over the "Index" pin
// TODO: only arm index edge interrupt when we know encoder has powered up
// (maybe by attaching the interrupt on start search, synergistic with following)
void Encoder::enc_index_cb() {
    if (config_.use_index) {
        set_circular_count(0, false);
        if (config_.use_index_offset)
            set_linear_count((int32_t)(config_.index_offset * config_.cpr));
        if (config_.pre_calibrated) {
            is_ready_ = true;
            if(axis_->controller_.config_.anticogging.pre_calibrated){
                axis_->controller_.anticogging_valid_ = true;
            }
        } else {
            // We can't use the update_offset facility in set_circular_count because
            // we also set the linear count before there is a chance to update. Therefore:
            // Invalidate offset calibration that may have happened before idx search
            is_ready_ = false;
        }
        index_found_ = true;
    }

    // Disable interrupt
    index_gpio_.unsubscribe();
}

void Encoder::set_idx_subscribe(bool override_enable) {
    if (config_.use_index && (override_enable || !config_.find_idx_on_lockin_only)) {
        if (!index_gpio_.subscribe(true, false, enc_index_cb_wrapper, this)) {
            odrv.misconfigured_ = true;
        }
    } else if (!config_.use_index || config_.find_idx_on_lockin_only) {
        index_gpio_.unsubscribe();
    }
}

void Encoder::update_pll_gains() {
    pll_kp_ = 2.0f * config_.bandwidth;  // basic conversion to discrete time
    pll_ki_ = 0.25f * (pll_kp_ * pll_kp_); // Critically damped

    // Check that we don't get problems with discrete time approximation
    if (!(current_meas_period * pll_kp_ < 1.0f)) {
        set_error(ERROR_UNSTABLE_GAIN);
    }
}

void Encoder::check_pre_calibrated() {
    // TODO: restoring config from python backup is fragile here (ACIM motor type must be set first)
    if (axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_ACIM) {
        if (!is_ready_)
            config_.pre_calibrated = false;
        if (mode_ == MODE_INCREMENTAL && !index_found_)
            config_.pre_calibrated = false;
    }
}

// Function that sets the current encoder count to a desired 32-bit value.
void Encoder::set_linear_count(int32_t count) {
    // Disable interrupts to make a critical section to avoid race condition
    uint32_t prim = cpu_enter_critical();

    // Update states
    shadow_count_ = count;
    pos_estimate_counts_ = (float)count;
    tim_cnt_sample_ = count;

    //Write hardware last
    timer_->Instance->CNT = count;

    cpu_exit_critical(prim);
}

// Function that sets the CPR circular tracking encoder count to a desired 32-bit value.
// Note that this will get mod'ed down to [0, cpr)
void Encoder::set_circular_count(int32_t count, bool update_offset) {
    // Disable interrupts to make a critical section to avoid race condition
    uint32_t prim = cpu_enter_critical();

    if (update_offset) {
        config_.phase_offset += count - count_in_cpr_;
        config_.phase_offset = mod(config_.phase_offset, config_.cpr);
    }

    // Update states
    count_in_cpr_ = mod(count, config_.cpr);
    pos_cpr_counts_ = (float)count_in_cpr_;

    cpu_exit_critical(prim);
}

bool Encoder::run_index_search() {
    config_.use_index = true;
    index_found_ = false;
    set_idx_subscribe();

    bool success = axis_->run_lockin_spin(axis_->config_.calibration_lockin, false);
    return success;
}

bool Encoder::run_direction_find() {
    int32_t init_enc_val = shadow_count_;

    Axis::LockinConfig_t lockin_config = axis_->config_.calibration_lockin;
    lockin_config.finish_distance = lockin_config.vel * 3.0f; // run for 3 seconds
    lockin_config.finish_on_distance = true;
    lockin_config.finish_on_enc_idx = false;
    lockin_config.finish_on_vel = false;
    bool success = axis_->run_lockin_spin(lockin_config, false);

    if (success) {
        // Check response and direction
        if (shadow_count_ > init_enc_val + 8) {
            // motor same dir as encoder
            config_.direction = 1;
        } else if (shadow_count_ < init_enc_val - 8) {
            // motor opposite dir as encoder
            config_.direction = -1;
        } else {
            config_.direction = 0;
        }
    }

    return success;
}


bool Encoder::run_hall_polarity_calibration() {
    Axis::LockinConfig_t lockin_config = axis_->config_.calibration_lockin;
    lockin_config.finish_distance = lockin_config.vel * 3.0f; // run for 3 seconds
    lockin_config.finish_on_distance = true;
    lockin_config.finish_on_enc_idx = false;
    lockin_config.finish_on_vel = false;

    auto loop_cb = [this](bool const_vel) {
        if (const_vel)
            sample_hall_states_ = true;
        // No need to cancel early
        return true;
    };

    config_.hall_polarity_calibrated = false;
    states_seen_count_.fill(0);
    bool success = axis_->run_lockin_spin(lockin_config, false, loop_cb);
    sample_hall_states_ = false;

    if (success) {
        std::bitset<8> state_seen;
        std::bitset<8> state_confirmed;
        for (int i = 0; i < 8; i++) {
            if (states_seen_count_[i] > 0)
                state_seen[i] = true;
            if (states_seen_count_[i] > 50)
                state_confirmed[i] = true;
        }
        if (!(state_seen == state_confirmed)) {
            set_error(ERROR_ILLEGAL_HALL_STATE);
            return false;
        }

        // Hall effect sensors can be arranged at 60 or 120 electrical degrees.
        // Out of 8 possible states, 120 and 60 deg arrangements each miss 2 states.
        // ODrive assumes 120 deg separation - if a 60 deg setup is used, it can
        // be converted to 120 deg states by flipping the polarity of one sensor.
        uint8_t states = state_seen.to_ulong();
        uint8_t hall_polarity = 0;
        auto flip_detect = [](uint8_t states, unsigned int idx)->bool {
            return (~states & 0xFF) == (1<<(0+idx) | 1<<(7-idx));
        };
        if (flip_detect(states, 0)) {
            hall_polarity = 0b000;
        } else if (flip_detect(states, 1)) {
            hall_polarity = 0b001;
        } else if (flip_detect(states, 2)) {
            hall_polarity = 0b010;
        } else if (flip_detect(states, 3)) {
            hall_polarity = 0b100;
        } else {
            set_error(ERROR_ILLEGAL_HALL_STATE);
            return false;
        }
        config_.hall_polarity = hall_polarity;
        config_.hall_polarity_calibrated = true;
    }

    return success;
}

bool Encoder::run_hall_phase_calibration() {
    Axis::LockinConfig_t lockin_config = axis_->config_.calibration_lockin;
    lockin_config.finish_distance = lockin_config.vel * 30.0f; // run for 30 seconds
    lockin_config.finish_on_distance = true;
    lockin_config.finish_on_enc_idx = false;
    lockin_config.finish_on_vel = false;

    auto loop_cb = [this](bool const_vel) {
        if (const_vel)
            sample_hall_phase_ = true;
        // No need to cancel early
        return true;
    };

    // TODO: There is a race condition here with the execution in Encoder::update.
    // We should evaluate making thread execution synchronous with the control loops
    // at least optionally.
    // Perhaps the new loop_sync feature will give a loose timing guarantee that may be sufficient
    calibrate_hall_phase_ = true;
    config_.hall_edge_phcnt.fill(0.0f);
    hall_phase_calib_seen_count_.fill(0);
    bool success = axis_->run_lockin_spin(lockin_config, false, loop_cb);
    if (error_ & ERROR_ILLEGAL_HALL_STATE)
        success = false;

    if (success) {
        // Check deltas to dicern rotation direction
        float delta_phase = 0.0f;
        for (int i = 0; i < 6; i++) {
            int next_i = (i == 5) ? 0 : i+1;
            delta_phase += wrap_pm_pi(config_.hall_edge_phcnt[next_i] - config_.hall_edge_phcnt[i]);
        }
        // Correct reverse rotation
        if (delta_phase < 0.0f) {
            config_.direction = -1;
            for (int i = 0; i < 6; i++)
                config_.hall_edge_phcnt[i] = wrap_pm_pi(-config_.hall_edge_phcnt[i]);
        } else {
            config_.direction = 1;
        }
        // Normalize edge timing to 1st edge in sequence, and change units to counts
        float offset = config_.hall_edge_phcnt[0];
        for (int i = 0; i < 6; i++) {
            float& phcnt = config_.hall_edge_phcnt[i];
            phcnt = fmodf_pos((6.0f / (2.0f * M_PI)) * (phcnt - offset), 6.0f);
        }
    } else {
        config_.hall_edge_phcnt = hall_edge_defaults;
    }

    calibrate_hall_phase_ = false;
    return success;
}

// @brief Turns the motor in one direction for a bit and then in the other
// direction in order to find the offset between the electrical phase 0
// and the encoder state 0.
bool Encoder::run_offset_calibration() {
    const float start_lock_duration = 1.0f;
    const bool is_as5600 = mode_ == MODE_I2C_ABS_AS5600;

    if (is_as5600) {
        // A saved offset/direction must not make an unhealthy absolute sensor
        // eligible for a new calibration or closed-loop run.
        is_ready_ = false;
        config_.pre_calibrated = false;
        config_.as5600_calibration_version = 0;
        if (!as5600_have_sample_ || sample_age_ >= 0.001f ||
            !as5600::magnet_detected(as5600_status_) || error_ != ERROR_NONE) {
            set_error(ERROR_NOT_READY);
            return false;
        }
    }

    // Require index found if enabled
    if (config_.use_index && !index_found_) {
        set_error(ERROR_INDEX_NOT_FOUND_YET);
        return false;
    }

    if (config_.mode == MODE_HALL && !config_.hall_polarity_calibrated) {
        set_error(ERROR_HALL_NOT_CALIBRATED_YET);
        return false;
    }

    // We use shadow_count_ to do the calibration, but the offset is used by count_in_cpr_
    // Therefore we have to sync them for calibration
    shadow_count_ = count_in_cpr_;

    CRITICAL_SECTION() {
        // Reset state variables
        axis_->open_loop_controller_.Idq_setpoint_ = {0.0f, 0.0f};
        axis_->open_loop_controller_.Vdq_setpoint_ = {0.0f, 0.0f};
        axis_->open_loop_controller_.phase_ = 0.0f;
        axis_->open_loop_controller_.phase_vel_ = 0.0f;

        float max_current_ramp = axis_->motor_.config_.calibration_current / start_lock_duration * 2.0f;
        axis_->open_loop_controller_.max_current_ramp_ = max_current_ramp;
        axis_->open_loop_controller_.max_voltage_ramp_ = max_current_ramp;
        axis_->open_loop_controller_.max_phase_vel_ramp_ = INFINITY;
        axis_->open_loop_controller_.target_current_ = axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_GIMBAL ? axis_->motor_.config_.calibration_current : 0.0f;
        axis_->open_loop_controller_.target_voltage_ = axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_GIMBAL ? 0.0f : axis_->motor_.config_.calibration_current;
        axis_->open_loop_controller_.target_vel_ = 0.0f;
        axis_->open_loop_controller_.total_distance_ = 0.0f;
        axis_->open_loop_controller_.phase_ = axis_->open_loop_controller_.initial_phase_ = wrap_pm_pi(0 - config_.calib_scan_distance / 2.0f);

        axis_->motor_.current_control_.enable_current_control_src_ = (axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_GIMBAL);
        axis_->motor_.current_control_.Idq_setpoint_src_.connect_to(&axis_->open_loop_controller_.Idq_setpoint_);
        axis_->motor_.current_control_.Vdq_setpoint_src_.connect_to(&axis_->open_loop_controller_.Vdq_setpoint_);
        
        axis_->motor_.current_control_.phase_src_.connect_to(&axis_->open_loop_controller_.phase_);
        axis_->acim_estimator_.rotor_phase_src_.connect_to(&axis_->open_loop_controller_.phase_);

        axis_->motor_.phase_vel_src_.connect_to(&axis_->open_loop_controller_.phase_vel_);
        axis_->motor_.current_control_.phase_vel_src_.connect_to(&axis_->open_loop_controller_.phase_vel_);
        axis_->acim_estimator_.rotor_phase_vel_src_.connect_to(&axis_->open_loop_controller_.phase_vel_);
    }
    axis_->wait_for_control_iteration();

    axis_->motor_.arm(&axis_->motor_.current_control_);

    // go to start position of forward scan for start_lock_duration to get ready to scan
    for (size_t i = 0; i < (size_t)(start_lock_duration * 1000.0f); ++i) {
        if (!axis_->motor_.is_armed_) {
            return false; // TODO: return "disarmed" error code
        }
        if (axis_->requested_state_ != Axis::AXIS_STATE_UNDEFINED) {
            axis_->motor_.disarm();
            return false; // TODO: return "aborted" error code
        }
        osDelay(1);
    }


    int32_t init_enc_val = shadow_count_;
    uint32_t num_steps = 0;
    int64_t encvaluesum = 0;

    CRITICAL_SECTION() {
        axis_->open_loop_controller_.target_vel_ = config_.calib_scan_omega;
        axis_->open_loop_controller_.total_distance_ = 0.0f;
    }

    // scan forward
    while ((axis_->requested_state_ == Axis::AXIS_STATE_UNDEFINED) && axis_->motor_.is_armed_) {
        bool reached_target_dist = axis_->open_loop_controller_.total_distance_.any().value_or(-INFINITY) >= config_.calib_scan_distance;
        if (reached_target_dist) {
            break;
        }
        encvaluesum += shadow_count_;
        num_steps++;
        osDelay(1);
    }

    // Check response and direction. Commit the result only after the reverse
    // scan has independently confirmed it.
    int32_t calibrated_direction = 0;
    if (shadow_count_ > init_enc_val + 8) {
        // motor same dir as encoder
        calibrated_direction = 1;
    } else if (shadow_count_ < init_enc_val - 8) {
        // motor opposite dir as encoder
        calibrated_direction = -1;
    } else {
        // Encoder response error
        set_error(ERROR_NO_RESPONSE);
        axis_->motor_.disarm();
        return false;
    }

    // Check CPR
    float elec_rad_per_enc = axis_->motor_.config_.pole_pairs * 2 * M_PI * (1.0f / (float)(config_.cpr));
    float expected_encoder_delta = config_.calib_scan_distance / elec_rad_per_enc;
    calib_scan_response_ = std::abs(shadow_count_ - init_enc_val);
    if (std::abs(calib_scan_response_ - expected_encoder_delta) / expected_encoder_delta > config_.calib_range) {
        set_error(ERROR_CPR_POLEPAIRS_MISMATCH);
        axis_->motor_.disarm();
        return false;
    }
    const int32_t forward_end_enc_val = shadow_count_;

    CRITICAL_SECTION() {
        axis_->open_loop_controller_.target_vel_ = -config_.calib_scan_omega;
    }

    // scan backwards
    while ((axis_->requested_state_ == Axis::AXIS_STATE_UNDEFINED) && axis_->motor_.is_armed_) {
        bool reached_target_dist = axis_->open_loop_controller_.total_distance_.any().value_or(INFINITY) <= 0.0f;
        if (reached_target_dist) {
            break;
        }
        encvaluesum += shadow_count_;
        num_steps++;
        osDelay(1);
    }

    // Motor disarmed because of an error
    if (!axis_->motor_.is_armed_) {
        return false;
    }

    axis_->motor_.disarm();

    if (num_steps == 0) {
        set_error(ERROR_NO_RESPONSE);
        return false;
    }

    // The reverse scan must move by the expected magnitude in the opposite
    // encoder direction and return close to the starting count. This catches
    // a transient direction result and a one-way/stalled sensor before any
    // calibration values are committed.
    const float reverse_delta = (float)(shadow_count_ - forward_end_enc_val);
    const float expected_signed_delta = calibrated_direction * expected_encoder_delta;
    const float response_tolerance = std::max(8.0f, expected_encoder_delta * config_.calib_range);
    if (is_as5600 &&
        (reverse_delta * calibrated_direction >= -8.0f ||
         std::abs(reverse_delta + expected_signed_delta) > response_tolerance ||
         std::abs((float)(shadow_count_ - init_enc_val)) > 2.0f * response_tolerance)) {
        set_error(ERROR_CPR_POLEPAIRS_MISMATCH);
        return false;
    }
    if (is_as5600 && (!as5600_have_sample_ || sample_age_ >= 0.001f ||
                      !as5600::magnet_detected(as5600_status_) ||
                      error_ != ERROR_NONE || as5600_consecutive_errors_ != 0)) {
        set_error(ERROR_NOT_READY);
        return false;
    }

    config_.phase_offset = encvaluesum / num_steps;
    int32_t residual = encvaluesum - ((int64_t)config_.phase_offset * (int64_t)num_steps);
    config_.phase_offset_float = (float)residual / (float)num_steps + 0.5f;  // add 0.5 to center-align state to phase
    config_.direction = calibrated_direction;
    if (is_as5600)
        config_.as5600_calibration_version = AS5600_CALIBRATION_VERSION;

    is_ready_ = true;
    return true;
}

static bool decode_hall(uint8_t hall_state, int32_t* hall_cnt) {
    switch (hall_state) {
        case 0b001: *hall_cnt = 0; return true;
        case 0b011: *hall_cnt = 1; return true;
        case 0b010: *hall_cnt = 2; return true;
        case 0b110: *hall_cnt = 3; return true;
        case 0b100: *hall_cnt = 4; return true;
        case 0b101: *hall_cnt = 5; return true;
        default: return false;
    }
}

void Encoder::sample_now() {
    switch (mode_) {
        case MODE_INCREMENTAL: {
            tim_cnt_sample_ = (int16_t)timer_->Instance->CNT;
        } break;

        case MODE_HALL: {
            // do nothing: samples already captured in general GPIO capture
        } break;

        case MODE_SINCOS: {
            sincos_sample_s_ = get_adc_relative_voltage(get_gpio(config_.sincos_gpio_pin_sin)) - 0.5f;
            sincos_sample_c_ = get_adc_relative_voltage(get_gpio(config_.sincos_gpio_pin_cos)) - 0.5f;
        } break;

        case MODE_SPI_ABS_AMS:
        case MODE_SPI_ABS_CUI:
        case MODE_SPI_ABS_AEAT:
        case MODE_SPI_ABS_RLS:
        case MODE_SPI_ABS_MA732:
        {
            abs_spi_start_transaction();
            // Do nothing
        } break;

        case MODE_I2C_ABS_AS5600: {
#if HW_VERSION_MAJOR == 3
            if (!i2c1_as5600_master)
                break;
            // One transaction slot every 250 us: seven RAW ANGLE reads then
            // one STATUS read. This yields 3.5 kHz angle and 500 Hz status
            // requests without placing STATUS immediately behind each angle.
            if ((as5600_poll_ticks_ & 1u) == 0u) {
                const uint8_t slot = (uint8_t)(as5600_poll_ticks_ >> 1);
                if (as5600::transaction_is_status(slot)) {
                    as5600_start_read(0x0b, (uint8_t*)as5600_status_buf_, 1, 2);
                } else {
                    as5600_start_read(0x0c, (uint8_t*)as5600_angle_buf_, 2, 1);
                }
            }
            as5600_poll_ticks_++;
#endif
        } break;

        default: {
           set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
        } break;
    }

    // Sample all GPIO digital input data registers, used for HALL sensors for example.
    for (size_t i = 0; i < sizeof(ports_to_sample) / sizeof(ports_to_sample[0]); ++i) {
        port_samples_[i] = ports_to_sample[i]->IDR;
    }
}

bool Encoder::read_sampled_gpio(Stm32Gpio gpio) {
    for (size_t i = 0; i < sizeof(ports_to_sample) / sizeof(ports_to_sample[0]); ++i) {
        if (ports_to_sample[i] == gpio.port_) {
            return port_samples_[i] & gpio.pin_mask_;
        }
    }
    return false;
}

void Encoder::decode_hall_samples() {
    hall_state_ = (read_sampled_gpio(hallA_gpio_) ? 1 : 0)
                | (read_sampled_gpio(hallB_gpio_) ? 2 : 0)
                | (read_sampled_gpio(hallC_gpio_) ? 4 : 0);
}

bool Encoder::abs_spi_start_transaction() {
    if (mode_ == MODE_SPI_ABS_RLS || mode_ == MODE_SPI_ABS_AMS ||
        mode_ == MODE_SPI_ABS_CUI || mode_ == MODE_SPI_ABS_AEAT ||
        mode_ == MODE_SPI_ABS_MA732){
        if (Stm32SpiArbiter::acquire_task(&spi_task_)) {
            spi_task_.ncs_gpio = abs_spi_cs_gpio_;
            spi_task_.tx_buf = (uint8_t*)abs_spi_dma_tx_;
            spi_task_.rx_buf = (uint8_t*)abs_spi_dma_rx_;
            spi_task_.length = 1;
            spi_task_.on_complete = [](void* ctx, bool success) { ((Encoder*)ctx)->abs_spi_cb(success); };
            spi_task_.on_complete_ctx = this;
            spi_task_.next = nullptr;
            
            spi_arbiter_->transfer_async(&spi_task_);
        } else {
            return false;
        }
    }
    return true;
}

uint8_t ams_parity(uint16_t v) {
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

uint8_t cui_parity(uint16_t v) {
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    return ~v & 3;
}

void Encoder::abs_spi_cb(bool success) {
    uint16_t pos;

    if (!success) {
        goto done;
    }

    switch (mode_) {
        case MODE_SPI_ABS_AMS: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            // check if parity is correct (even) and error flag clear
            if (ams_parity(rawVal) || ((rawVal >> 14) & 1)) {
                goto done;
            }
            pos = rawVal & 0x3fff;
        } break;

        case MODE_SPI_ABS_CUI: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            // check if parity is correct
            if (cui_parity(rawVal)) {
                goto done;
            }
            pos = rawVal & 0x3fff;
        } break;

        case MODE_SPI_ABS_RLS: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            pos = (rawVal >> 2) & 0x3fff;
        } break;

        case MODE_SPI_ABS_MA732: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            pos = (rawVal >> 2) & 0x3fff;
        } break;

        default: {
           set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
           goto done;
        } break;
    }

    pos_abs_ = pos;
    abs_spi_pos_updated_ = true;
    if (config_.pre_calibrated) {
        is_ready_ = true;
    }

done:
    Stm32SpiArbiter::release_task(&spi_task_);
}

void Encoder::abs_spi_cs_pin_init(){
    // Decode and init cs pin
#if HW_VERSION_MAJOR == 4
    if (mode_ == MODE_SPI_ABS_MA732)
        abs_spi_cs_gpio_ = {GPIOA, GPIO_PIN_15};
    else
#else
    abs_spi_cs_gpio_ = get_gpio(config_.abs_spi_cs_gpio_pin);
#endif
    abs_spi_cs_gpio_.config(GPIO_MODE_OUTPUT_PP, GPIO_PULLUP);

    // Write pin high
    abs_spi_cs_gpio_.write(true);
}

// Note that this may return counts +1 or -1 without any wrapping
int32_t Encoder::hall_model(float internal_pos) {
    int32_t base_cnt = (int32_t)std::floor(internal_pos);

    float pos_in_range = fmodf_pos(internal_pos, 6.0f);
    int pos_idx = (int)pos_in_range;
    if (pos_idx == 6) pos_idx = 5; // in case of rounding error
    int next_i = (pos_idx == 5) ? 0 : pos_idx+1;

    float below_edge = config_.hall_edge_phcnt[pos_idx];
    float above_edge = config_.hall_edge_phcnt[next_i];

    // if we are blow the "below" edge, we are the count under
    if (wrap_pm(pos_in_range - below_edge, 6.0f) < 0.0f)
        return base_cnt - 1;
    // if we are above the "above" edge, we are the count over
    else if (wrap_pm(pos_in_range - above_edge, 6.0f) > 0.0f)
        return base_cnt + 1;
    // otherwise we are in the nominal count (or completely lost)
    return base_cnt;
}

bool Encoder::update() {
    // update internal encoder state.
    int32_t delta_enc = 0;
    int32_t pos_abs_latched = pos_abs_; //LATCH
    bool as5600_new_sample = false;

    if (mode_ == MODE_I2C_ABS_AS5600) {
        uint32_t sample_seq;
        uint32_t error_seq;
        uint32_t status_seq;
        uint32_t complete_seq;
        uint32_t failed_seq;
        uint16_t raw;
        uint8_t status;
        uint32_t last_sample_tick;
        uint32_t last_status_tick;
        bool have_status;
        uint32_t busy_skip_seq;
        uint8_t irq_consecutive_errors;
        uint32_t prim = cpu_enter_critical();
        sample_seq = as5600_sample_seq_;
        error_seq = as5600_error_seq_;
        status_seq = as5600_status_seq_;
        complete_seq = as5600_complete_seq_;
        failed_seq = as5600_failed_seq_;
        raw = as5600::decode_raw_angle(as5600_angle_buf_[0], as5600_angle_buf_[1]);
        status = as5600_status_buf_[0];
        last_sample_tick = as5600_last_sample_tick_;
        last_status_tick = as5600_last_status_tick_;
        have_status = as5600_have_status_;
        busy_skip_seq = as5600_busy_skip_seq_;
        irq_consecutive_errors = as5600_irq_consecutive_errors_;
        cpu_exit_critical(prim);
        as5600_busy_skip_count_ = busy_skip_seq;
        as5600_consecutive_errors_ = irq_consecutive_errors;
        if (status_seq != as5600_seen_status_seq_) {
            as5600_seen_status_seq_ = status_seq;
            as5600_status_ = status;
            if (!as5600::magnet_detected(status)) {
                if (++as5600_bad_magnet_count_ >= 2)
                    set_error(ERROR_ABS_I2C_MAGNET_ERROR);
            } else {
                as5600_bad_magnet_count_ = 0;
            }
        }
        if (sample_seq != as5600_seen_sample_seq_) {
            as5600_seen_sample_seq_ = sample_seq;
            as5600_new_sample = true;
            if (!as5600_have_sample_) {
                pos_abs_ = raw;
                count_in_cpr_ = raw;
                shadow_count_ = raw;
                pos_estimate_counts_ = (float)raw;
                pos_cpr_counts_ = (float)raw;
                as5600_have_sample_ = true;
                as5600_first_sample_ = true;
                if (config_.pre_calibrated &&
                    config_.as5600_calibration_version == AS5600_CALIBRATION_VERSION)
                    is_ready_ = true;
            } else {
                pos_abs_ = raw;
            }
        }
        if (error_seq != as5600_seen_error_seq_) {
            as5600_seen_error_seq_ = error_seq;
            if (as5600::communication_failed(irq_consecutive_errors)) {
                set_error(ERROR_ABS_I2C_COM_FAIL);
                as5600_recovery_requested_ = true;
            }
        }
        if (complete_seq != as5600_seen_complete_seq_) {
            uint32_t complete_delta = complete_seq - as5600_seen_complete_seq_;
            uint32_t failed_delta = failed_seq - as5600_seen_failed_seq_;
            float transaction_error = complete_delta ?
                (float)failed_delta / (float)complete_delta : 1.0f;
            i2c_error_rate_ += 0.05f * (transaction_error - i2c_error_rate_);
            as5600_seen_complete_seq_ = complete_seq;
            as5600_seen_failed_seq_ = failed_seq;
        }
        const uint32_t sample_reference_tick = as5600_have_sample_ ?
            last_sample_tick : as5600_monitor_start_tick_;
        sample_age_ = (odrv.n_evt_sampling_ - sample_reference_tick) * current_meas_period;
        if (!as5600_recovery_blocked_ &&
            as5600::sample_is_stale(odrv.n_evt_sampling_, sample_reference_tick)) {
            set_error(ERROR_ABS_I2C_COM_FAIL);
            as5600_recovery_requested_ = true;
        }
        if (!as5600_recovery_blocked_ && as5600_have_sample_ &&
            (!have_status ||
             as5600::status_is_stale(odrv.n_evt_sampling_, last_status_tick))) {
            set_error(ERROR_ABS_I2C_COM_FAIL);
            as5600_recovery_requested_ = true;
        }

        const uint32_t rate_elapsed = odrv.n_evt_sampling_ - as5600_rate_tick_;
        if (rate_elapsed >= 8000u) {
            const float seconds = rate_elapsed * current_meas_period;
            as5600_angle_sample_rate_ =
                (sample_seq - as5600_rate_angle_seq_) / seconds;
            as5600_status_sample_rate_ =
                (status_seq - as5600_rate_status_seq_) / seconds;
            as5600_rate_angle_seq_ = sample_seq;
            as5600_rate_status_seq_ = status_seq;
            as5600_rate_tick_ = odrv.n_evt_sampling_;
        }
    }

    switch (mode_) {
        case MODE_INCREMENTAL: {
            //TODO: use count_in_cpr_ instead as shadow_count_ can overflow
            //or use 64 bit
            int16_t delta_enc_16 = (int16_t)tim_cnt_sample_ - (int16_t)shadow_count_;
            delta_enc = (int32_t)delta_enc_16; //sign extend
        } break;

        case MODE_HALL: {
            decode_hall_samples();
            if (sample_hall_states_) {
                states_seen_count_[hall_state_]++;
            }
            if (config_.hall_polarity_calibrated) {
                int32_t hall_cnt;
                if (decode_hall((hall_state_ ^ config_.hall_polarity), &hall_cnt)) {
                    if (calibrate_hall_phase_) {
                        if (sample_hall_phase_ && last_hall_cnt_.has_value()) {
                            int mod_hall_cnt = mod(hall_cnt - last_hall_cnt_.value(), 6);
                            size_t edge_idx;
                            if (mod_hall_cnt == 0) { goto skip; } // no count - do nothing
                            else if (mod_hall_cnt == 1) { // counted up
                                edge_idx = hall_cnt;
                            } else if (mod_hall_cnt == 5) { // counted down
                                edge_idx = last_hall_cnt_.value();
                            } else {
                                set_error(ERROR_ILLEGAL_HALL_STATE);
                                return false;
                            }

                            auto maybe_phase = axis_->open_loop_controller_.phase_.any();
                            if (maybe_phase) {
                                float phase = maybe_phase.value();
                                // Early increment to get the right divisor in recursive average
                                hall_phase_calib_seen_count_[edge_idx]++;
                                float& edge_phase = config_.hall_edge_phcnt[edge_idx];
                                if (hall_phase_calib_seen_count_[edge_idx] == 1)
                                    edge_phase = phase;
                                else {
                                    // circularly wrapped recursive average
                                    edge_phase += (phase - edge_phase) / hall_phase_calib_seen_count_[edge_idx];
                                    edge_phase = wrap_pm_pi(edge_phase);
                                }
                            }
                        }
                    skip:
                        last_hall_cnt_ = hall_cnt;

                        return true; // Skip all velocity and phase estimation
                    }

                    delta_enc = hall_cnt - count_in_cpr_;
                    delta_enc = mod(delta_enc, 6);
                    if (delta_enc > 3)
                        delta_enc -= 6;
                } else {
                    if (!config_.ignore_illegal_hall_state) {
                        set_error(ERROR_ILLEGAL_HALL_STATE);
                        return false;
                    }
                }
            }
        } break;

        case MODE_SINCOS: {
            float phase = fast_atan2(sincos_sample_s_, sincos_sample_c_);
            int fake_count = (int)(1000.0f * phase);
            //CPR = 6283 = 2pi * 1k

            delta_enc = fake_count - count_in_cpr_;
            delta_enc = mod(delta_enc, 6283);
            if (delta_enc > 6283/2)
                delta_enc -= 6283;
        } break;
        
        case MODE_SPI_ABS_RLS:
        case MODE_SPI_ABS_AMS:
        case MODE_SPI_ABS_CUI: 
        case MODE_SPI_ABS_AEAT:
        case MODE_SPI_ABS_MA732: {
            if (abs_spi_pos_updated_ == false) {
                // Low pass filter the error
                spi_error_rate_ += current_meas_period * (1.0f - spi_error_rate_);
                if (spi_error_rate_ > 0.05f) {
                    set_error(ERROR_ABS_SPI_COM_FAIL);
                    return false;
                }
            } else {
                // Low pass filter the error
                spi_error_rate_ += current_meas_period * (0.0f - spi_error_rate_);
            }

            abs_spi_pos_updated_ = false;
            delta_enc = pos_abs_latched - count_in_cpr_; //LATCH
            delta_enc = mod(delta_enc, config_.cpr);
            if (delta_enc > config_.cpr/2) {
                delta_enc -= config_.cpr;
            }

        }break;
        case MODE_I2C_ABS_AS5600: {
            if (!as5600_have_sample_) {
                delta_enc = 0;
            } else if (as5600_first_sample_) {
                delta_enc = 0;
                as5600_first_sample_ = false;
                pos_abs_latched = pos_abs_;
            } else {
                if (as5600_new_sample)
                    pos_abs_latched = pos_abs_;
                if (as5600_resume_after_recovery_ && as5600_new_sample) {
                    const int32_t resumed = as5600::resume_multiturn(
                        shadow_count_, (uint16_t)pos_abs_latched,
                        (uint16_t)count_in_cpr_);
                    delta_enc = resumed - shadow_count_;
                    as5600_resume_after_recovery_ = false;
                } else {
                    delta_enc = as5600::wrapped_delta(pos_abs_latched, count_in_cpr_);
                }
            }
        } break;
        default: {
            set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
            return false;
        } break;
    }

    shadow_count_ += delta_enc;
    count_in_cpr_ += delta_enc;
    count_in_cpr_ = mod(count_in_cpr_, config_.cpr);

    if((mode_ & MODE_FLAG_ABS) && mode_ != MODE_I2C_ABS_AS5600)
        count_in_cpr_ = pos_abs_latched;

    // Memory for pos_circular
    float pos_cpr_counts_last = pos_cpr_counts_;

    //// run pll (for now pll is in units of encoder counts)
    // Predict current pos
    pos_estimate_counts_ += current_meas_period * vel_estimate_counts_;
    pos_cpr_counts_      += current_meas_period * vel_estimate_counts_;
    // Encoder model
    auto encoder_model = [this](float internal_pos)->int32_t {
        if (config_.mode == MODE_HALL)
            return hall_model(internal_pos);
        else
            return (int32_t)std::floor(internal_pos);
    };
    // discrete phase detector
    float delta_pos_counts = (float)(shadow_count_ - encoder_model(pos_estimate_counts_));
    float delta_pos_cpr_counts = (float)(count_in_cpr_ - encoder_model(pos_cpr_counts_));
    delta_pos_cpr_counts = wrap_pm(delta_pos_cpr_counts, (float)(config_.cpr));
    delta_pos_cpr_counts_ += 0.1f * (delta_pos_cpr_counts - delta_pos_cpr_counts_); // for debug
    // pll feedback
    pos_estimate_counts_ += current_meas_period * pll_kp_ * delta_pos_counts;
    pos_cpr_counts_ += current_meas_period * pll_kp_ * delta_pos_cpr_counts;
    pos_cpr_counts_ = fmodf_pos(pos_cpr_counts_, (float)(config_.cpr));
    vel_estimate_counts_ += current_meas_period * pll_ki_ * delta_pos_cpr_counts;
    bool snap_to_zero_vel = false;
    if (std::abs(vel_estimate_counts_) < 0.5f * current_meas_period * pll_ki_) {
        vel_estimate_counts_ = 0.0f;  //align delta-sigma on zero to prevent jitter
        snap_to_zero_vel = true;
    }

    // Outputs from Encoder for Controller
    pos_estimate_ = pos_estimate_counts_ / (float)config_.cpr;
    vel_estimate_ = vel_estimate_counts_ / (float)config_.cpr;
    
    // TODO: we should strictly require that this value is from the previous iteration
    // to avoid spinout scenarios. However that requires a proper way to reset
    // the encoder from error states.
    float pos_circular = pos_circular_.any().value_or(0.0f);
    pos_circular +=  wrap_pm((pos_cpr_counts_ - pos_cpr_counts_last) / (float)config_.cpr, 1.0f);
    pos_circular = fmodf_pos(pos_circular, axis_->controller_.config_.circular_setpoint_range);
    pos_circular_ = pos_circular;

    //// run encoder count interpolation
    int32_t corrected_enc = count_in_cpr_ - config_.phase_offset;
    // if we are stopped, make sure we don't randomly drift
    if (snap_to_zero_vel || !config_.enable_phase_interpolation) {
        interpolation_ = 0.5f;
    // reset interpolation if encoder edge comes
    // TODO: This isn't correct. At high velocities the first phase in this count may very well not be at the edge.
    } else if (delta_enc > 0) {
        interpolation_ = 0.0f;
    } else if (delta_enc < 0) {
        interpolation_ = 1.0f;
    } else {
        // Interpolate (predict) between encoder counts using vel_estimate,
        interpolation_ += current_meas_period * vel_estimate_counts_;
        // don't allow interpolation indicated position outside of [enc, enc+1)
        if (interpolation_ > 1.0f) interpolation_ = 1.0f;
        if (interpolation_ < 0.0f) interpolation_ = 0.0f;
    }
    float interpolated_enc = corrected_enc + interpolation_;

    //// compute electrical phase
    //TODO avoid recomputing elec_rad_per_enc every time
    float elec_rad_per_enc = axis_->motor_.config_.pole_pairs * 2 * M_PI * (1.0f / (float)(config_.cpr));
    float ph = elec_rad_per_enc * (interpolated_enc - config_.phase_offset_float);
    if (mode_ == MODE_I2C_ABS_AS5600) {
        float delay = config_.phase_delay_compensation;
        if (!std::isfinite(delay) || delay < 0.0f || delay > 0.005f) {
            odrv.misconfigured_ = true;
            delay = 0.0004f;
        }
        float vel = vel_estimate_.any().value_or(0.0f);
        ph += 2.0f * M_PI * vel * axis_->motor_.config_.pole_pairs * delay;
    }
    
    if (is_ready_) {
        phase_ = wrap_pm_pi(ph) * config_.direction;
        phase_vel_ = (2*M_PI) * *vel_estimate_.present() * axis_->motor_.config_.pole_pairs * config_.direction;
    }

    return true;
}
