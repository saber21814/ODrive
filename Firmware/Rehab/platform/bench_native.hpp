// Included only inside runtime.cpp's private namespace in the dedicated bench
// build. Native algorithms are called through their existing public interfaces.
using BenchReason = rehab::bench::Reason;
using BenchState = rehab::bench::State;
bool bench_motor_passed=false,bench_encoder_passed=false,bench_target_applied=false;
bool bench_boot_blocked=false;
bool bench_open_loop_configured=false;
bool bench_encoder_guard_overridden=false,bench_saved_overspeed=true;
int32_t bench_saved_direction=0;
Axis::LockinConfig_t bench_saved_lockin;
void bench_restore_test_config() {
    if(bench_open_loop_configured) {
        encoders[0].config_.direction=bench_saved_direction;
        axes[0].config_.general_lockin=bench_saved_lockin;
        bench_open_loop_configured=false;
    }
    if(bench_encoder_guard_overridden) {
        axes[0].controller_.config_.enable_overspeed_error=bench_saved_overspeed;
        bench_encoder_guard_overridden=false;
    }
}
void bench_revoke_on_trip(uint32_t why) {
    if(bench_session.state!=BenchState::Fault)bench_session.fail(why==FaultStop?BenchReason::Stop:BenchReason::Native);
    bench_motor_passed=bench_encoder_passed=false;
    if(bench_open_loop_configured || bench_encoder_guard_overridden){motors[0].disarm();bench_restore_test_config();}
    axes[0].config_.enable_watchdog=false;
    auto& c=axes[0].controller_;c.input_torque_=c.input_vel_=c.torque_setpoint_=c.vel_integrator_torque_=0.0f;
    c.torque_output_=0.0f;
}
uint32_t bench_adc_errors=0,bench_gate_generation=0;

void bench_quiesce() {
    motors[0].disarm();
    bench_restore_test_config();
    axes[0].config_.enable_watchdog=false;
    if(axes[0].current_state_!=Axis::AXIS_STATE_IDLE || axes[0].requested_state_!=Axis::AXIS_STATE_UNDEFINED)
        axes[0].requested_state_=Axis::AXIS_STATE_IDLE;
    auto& c=axes[0].controller_;
    c.input_torque_=c.input_vel_=c.torque_setpoint_=c.vel_integrator_torque_=0.0f;
    c.torque_output_=0.0f;
}
void bench_fail(BenchReason reason) {
    bench_session.fail(reason);
    bench_motor_passed=bench_encoder_passed=false;
    bench_quiesce();
    trip(FaultPlatform,false);
}
void bench_boot_defaults() {
    // This image has its own NVM CRC version and never saves configuration.
    // Refuse even a matching persisted bench configuration rather than trusting
    // arbitrary saved GPIO modes applied later by native startup.
    bench_boot_blocked=odrv.user_config_loaded_;
    auto& a=axes[0];auto& m=motors[0];auto& e=encoders[0];auto& c=axes[0].controller_;
    // Placement construction avoids a 14 kB temporary anticogging map on stack.
    new (&m.config_) Motor::Config_t{};
    new (&e.config_) Encoder::Config_t{};
    new (&c.config_) Controller::Config_t{};
    new (&a.config_) Axis::Config_t{};
    m.config_.motor_type=Motor::MOTOR_TYPE_HIGH_CURRENT;
    m.config_.pole_pairs=4;m.config_.torque_constant=0.04f;
    m.config_.calibration_current=4.0f;m.config_.resistance_calib_max_voltage=3.0f;
    m.config_.requested_current_range=60.0f;m.config_.current_lim=5.0f;
    m.config_.current_lim_margin=1.0f;m.config_.torque_lim=0.2f;
    m.config_.I_bus_hard_min=-0.01f;m.config_.I_bus_hard_max=1.0f;
    m.config_.pre_calibrated=false;m.apply_config();m.is_calibrated_=false;
    e.config_.mode=Encoder::MODE_INCREMENTAL;e.config_.cpr=4000;
    e.config_.use_index=false;e.config_.pre_calibrated=false;
    e.apply_config(m.config_.motor_type);e.is_ready_=false;
    c.config_.load_encoder_axis=0;c.config_.vel_limit=0.3f;c.config_.vel_limit_tolerance=2.0f;
    c.config_.vel_ramp_rate=0.3f;c.config_.vel_integrator_limit=0.12f;
    c.config_.enable_torque_mode_vel_limit=false;
    c.config_.anticogging.anticogging_enabled=false;c.config_.anticogging.pre_calibrated=false;
    c.config_.anticogging.calib_anticogging=false;c.anticogging_valid_=false;
    c.apply_config();c.reset();
    a.config_.parent=&a;a.config_.watchdog_timeout=0.5f;a.config_.enable_watchdog=false;
    a.config_.enable_step_dir=false;a.config_.step_dir_always_on=false;a.step_dir_active_=false;
    a.min_endstop_.config_.enabled=false;a.max_endstop_.config_.enabled=false;
    a.mechanical_brake_.config_.gpio_num=0;
    fet_thermistors[0].config_.enabled=true;
    fet_thermistors[0].config_.temp_limit_lower=45.0f;fet_thermistors[0].config_.temp_limit_upper=50.0f;
    m.motor_thermistor_.config_.enabled=false;
    odrv.config_.dc_bus_undervoltage_trip_level=20.0f;odrv.config_.dc_bus_overvoltage_trip_level=28.0f;
    odrv.config_.dc_max_positive_current=1.0f;odrv.config_.dc_max_negative_current=-0.01f;
    odrv.config_.enable_brake_resistor=false;odrv.config_.brake_resistance=0.47f;
    odrv.config_.enable_dc_bus_overvoltage_ramp=false;
    pending_network=drivers::NetworkConfig{{192,168,11,2},{0,0,0,0},{255,255,255,0},39400};
    ++network_revision;
}
bool bench_dc_ready() {
    const auto& m=motors[0];const auto& d=m.DC_calib_;
    return std::isfinite(m.max_dc_calib_) && m.max_dc_calib_>0.0f &&
        m.dc_calib_running_since_>=m.config_.dc_calib_tau*7.5f &&
        std::isfinite(d.phA) && std::isfinite(d.phB) && std::isfinite(d.phC) &&
        std::fabs(d.phA)<m.max_dc_calib_ && std::fabs(d.phB)<m.max_dc_calib_ && std::fabs(d.phC)<m.max_dc_calib_ &&
        std::isfinite(m.phase_current_rev_gain_) && std::fabs(m.phase_current_rev_gain_-0.025f)<0.000001f;
}
BenchReason bench_health(bool starting) {
    if(bench_boot_blocked)return BenchReason::Prerequisite;
    if(stop_asserted() || !board_access::gate_ready())return BenchReason::Stop;
    if(latched || odrv.any_error())return BenchReason::Native;
    if(!std::isfinite(vbus_voltage) || vbus_voltage<(starting?22.0f:20.0f) || vbus_voltage>(starting?26.0f:28.0f))return BenchReason::Bus;
    if(!std::isfinite(fet_temp) || !std::isfinite(aux_temp) || fet_temp< -10.0f || aux_temp< -10.0f || fet_temp>=50.0f || aux_temp>=50.0f)return BenchReason::Temperature;
    const uint32_t adc=rehab_board_phase_adc_word(1);
    if((adc&0x87000000u)!=0x87000000u || rehab_board_phase_adc_word(2)>10 ||
       (starting && (adc&0x08000000u)) || !bench_dc_ready())return BenchReason::Adc;
    if(!starting && (rehab_board_phase_adc_word(3)!=bench_adc_errors || board_access::gate_generation()!=bench_gate_generation))return BenchReason::Adc;
    return BenchReason::None;
}
void bench_finish() {
    const auto phase=bench_session.stage;
    if(bench_session.state==BenchState::Complete) {
        if(phase==1) {
            const auto& m=motors[0];
            if(!m.is_calibrated_ || !std::isfinite(m.config_.phase_resistance) ||
               !(m.config_.calibration_current>0.0f) ||
               !(m.config_.phase_resistance>0.0f && m.config_.phase_resistance<=
                 1.05f*m.config_.resistance_calib_max_voltage/m.config_.calibration_current) ||
               !std::isfinite(m.config_.phase_inductance) ||
               !(m.config_.phase_inductance>=0.000002f && m.config_.phase_inductance<=0.004f)) {
                bench_fail(BenchReason::Prerequisite);return;
            }
            bench_motor_passed=true;
        } else if(phase==2) {
            if(!encoders[0].is_ready_ || std::abs(encoders[0].config_.direction)!=1){bench_fail(BenchReason::Prerequisite);return;}
            bench_encoder_passed=true;
        } else if(phase==5) {
            const float p=encoders[0].pos_estimate_.any().value_or(NAN);
            const float v=encoders[0].vel_estimate_.any().value_or(NAN);
            if(!bench_target_applied || !std::isfinite(p) || !std::isfinite(v) ||
               std::fabs(p-bench_session.start_position-bench_session.argument)>0.02f || std::fabs(v)>0.05f) {
                bench_fail(BenchReason::Position);return;
            }
        }
    }
    bench_quiesce();
    if(bench_session.state==BenchState::Fault) {
        bench_motor_passed=bench_encoder_passed=false;trip(FaultPlatform,false);
    }
    ++owner_epoch;command_boundary();
}
void bench_advance() {
    if(!bench_session.active())return;
    const auto reason=bench_health(false);
    if(reason!=BenchReason::None){bench_fail(reason);return;}
    // Motor R/L measurement and electrical-angle open loop need no encoder.
    // An absent encoder's zero count is NOT a measured speed/travel interlock.
    if(bench_session.stage>=2 && bench_session.stage<=5) {
        auto& e=encoders[0];
        const float p=e.pos_estimate_.any().value_or(NAN),v=e.vel_estimate_.any().value_or(NAN);
        if(!std::isfinite(p) || !std::isfinite(v)){bench_fail(BenchReason::Numeric);return;}
        if(std::fabs(v)>(bench_session.stage==2?1.0f:0.6f)){bench_fail(BenchReason::Speed);return;}
        if(bench_session.stage>=3 && std::fabs(p-bench_session.start_position)>
            (bench_session.stage==3?0.25f:bench_session.stage==4?3.5f:0.75f)){bench_fail(BenchReason::Travel);return;}
    }
    auto& a=axes[0];
    uint32_t observed_state=a.current_state_;
    // The native idle task rotates [IDLE, UNDEFINED, ...] before loading a new
    // request. Permit only that unarmed pending window; never alter the native
    // task chain and never authorize arm in UNDEFINED (bench_permit stays strict).
    if(observed_state==Axis::AXIS_STATE_UNDEFINED && bench_session.state==BenchState::Pending &&
       !bench_session.entered && !motors[0].is_armed_ &&
       uint32_t(a.requested_state_)==bench_session.target_state())observed_state=Axis::AXIS_STATE_IDLE;
    bench_session.poll(HAL_GetTick(),observed_state,motors[0].is_armed_);
    if(!bench_session.active())bench_finish();
}
void bench_control() {
    bench_advance();
    if(!bench_session.active())return;
    axes[0].watchdog_feed(); // Only while independent lease and total deadline are valid.
    if(bench_session.stage>=3 && bench_session.stage<=5 && motors[0].is_armed_ && axes[0].current_state_==Axis::AXIS_STATE_CLOSED_LOOP_CONTROL && !bench_target_applied) {
        auto& c=axes[0].controller_;
        c.input_torque_=0.0f;c.input_vel_=bench_session.stage==4?bench_session.argument:0.0f;
        c.input_pos_=bench_session.start_position+(bench_session.stage==5?bench_session.argument:0.0f);
        c.input_pos_updated();bench_target_applied=true;
    }
}
bool bench_permit() {
    return bench_session.permitted(HAL_GetTick()) && !latched && !stop_asserted() &&
        board_access::gate_ready() && !owner_session && !odrv.any_error() &&
        uint32_t(axes[0].current_state_)==bench_session.target_state();
}
void bench_write_status(wire::Writer& w) {
    const auto& a=axes[0];const auto& m=motors[0];auto& e=encoders[0];
    const uint32_t now=HAL_GetTick();const uint64_t me=uint64_t(m.error_);
    const uint32_t words[]={1,1,uint32_t(bench_session.active()),bench_session.stage,uint32_t(bench_session.state),uint32_t(bench_session.reason),
        bench_session.epoch,bench_session.remaining(now),now-bench_session.fed_ms,uint32_t(a.current_state_),uint32_t(m.is_armed_),
        uint32_t(m.is_calibrated_),uint32_t(e.is_ready_),uint32_t(bench_motor_passed),uint32_t(bench_encoder_passed),
        uint32_t(odrv.error_),uint32_t(a.error_),uint32_t(me),uint32_t(me>>32),uint32_t(e.error_),uint32_t(axes[0].controller_.error_),
        uint32_t(e.shadow_count_),uint32_t(e.config_.cpr),uint32_t(board_access::gate_ready()),uint32_t(stop_asserted()),
        rehab_board_phase_adc_word(1),rehab_board_phase_adc_word(2),rehab_board_phase_adc_word(3),
        diagnostic_word(36),diagnostic_word(38),diagnostic_word(22),diagnostic_word(21),diagnostic_word(67),diagnostic_word(66),diagnostic_word(32)};
    static_assert(sizeof(words)==35*4,"bench status words");
    for(auto value:words)w.word(value);
    const float values[]={vbus_voltage,e.pos_estimate_.any().value_or(NAN),e.vel_estimate_.any().value_or(NAN),
        m.current_control_.Iq_measured_,m.current_control_.Id_measured_,m.config_.phase_resistance,m.config_.phase_inductance,
        m.DC_calib_.phB,m.DC_calib_.phC,fet_temp,aux_temp,bench_session.argument};
    static_assert(sizeof(values)==12*4,"bench status floats");
    for(auto value:values)w.real(value);
}
Result bench_handle(void*,unsigned port,uint16_t command,uint32_t session,const uint8_t* payload,size_t length,wire::Writer& w) {
    const bool start_command=command==22;
    if(start_command?(length!=8 && length!=12 && length!=16):length!=0)return Result::InvalidConfig;
    CRITICAL_SECTION() {
        bench_advance();
        if(command==25){bench_write_status(w);return Result::Ok;}
        if(command==24){bench_session.stop();bench_quiesce();++owner_epoch;command_boundary();bench_write_status(w);return Result::Ok;}
        if(command==26) {
            if(bench_session.active() || !quiescent_axis() || stop_asserted())return Result::Busy;
            odrv.clear_errors();
            if(odrv.any_error() || latched)return Result::Faulted;
            bench_session.clear();bench_write_status(w);return Result::Ok;
        }
        if(command==23) {
            if(!bench_session.owns(port,session))return Result::NotOwner;
            if(!bench_session.feed(port,session,HAL_GetTick()))return Result::Faulted;
            axes[0].watchdog_feed();bench_write_status(w);return Result::Ok;
        }
        if(command!=22)return Result::Unsupported;
        uint32_t phase=wire::u32(payload);float value;std::memcpy(&value,payload+4,4);
        if(!rehab::bench::Session::valid(phase,value))return Result::InvalidConfig;
        float open_current=0.0f,cal_voltage=3.0f;
        uint32_t run_ms=1000;
        if(phase==6) {
            if(length!=16)return Result::InvalidConfig;
            std::memcpy(&open_current,payload+8,4);run_ms=wire::u32(payload+12);
            if(!std::isfinite(open_current) || open_current<0.1f || open_current>1.5f ||
               run_ms<200 || run_ms>5000)return Result::InvalidConfig;
        } else if(phase==1 && length==12) {
            std::memcpy(&cal_voltage,payload+8,4);
            if(!std::isfinite(cal_voltage) || cal_voltage<0.1f || cal_voltage>3.0f)return Result::InvalidConfig;
        } else if(length!=8)return Result::InvalidConfig;
        if(bench_session.active() || !quiescent_axis() || owner_session || native_writers)return Result::Busy;
        if(bench_session.state==BenchState::Fault)return Result::Faulted;
        if(dispatching_command && dispatch_fault_generation!=fault_generation)return Result::Stale;
        const auto health=bench_health(true);
        if(health!=BenchReason::None){bench_session.reason=health;return Result::Inhibited;}
        if((phase>=2 && !bench_motor_passed) || (phase>=3 && phase<=5 && !bench_encoder_passed) ||
           (phase==6 && !motors[0].is_calibrated_)){
            bench_session.reason=BenchReason::Prerequisite;return Result::Inhibited;
        }
        const bool uses_encoder=phase>=2 && phase<=5;
        const float p=uses_encoder?encoders[0].pos_estimate_.any().value_or(NAN):0.0f;
        const float v=uses_encoder?encoders[0].vel_estimate_.any().value_or(NAN):0.0f;
        if(!std::isfinite(p) || !std::isfinite(v) || std::fabs(v)>0.02f){
            bench_session.reason=BenchReason::Prerequisite;return Result::Inhibited;
        }
        if(!bench_session.begin(phase,value,port,session,HAL_GetTick(),p,run_ms))return Result::Inhibited;
        auto& m=motors[0];auto& e=encoders[0];auto& c=axes[0].controller_;auto& a=axes[0];
        if(!uses_encoder) {
            // Controller::update runs even outside closed loop and can retain a
            // velocity source from an earlier closed-loop test. With no feedback
            // this is not an actual speed interlock. Restore it after disarm.
            bench_saved_overspeed=c.config_.enable_overspeed_error;
            bench_encoder_guard_overridden=true;c.config_.enable_overspeed_error=false;
        }
        if(phase==1){bench_motor_passed=bench_encoder_passed=false;m.is_calibrated_=false;e.is_ready_=false;}
        if(phase==2){bench_encoder_passed=false;e.is_ready_=false;}
        m.config_.current_lim=phase<=2?5.0f:phase==3?1.0f:phase==6?open_current:3.0f;
        // Native idle measurements contain the negative DC offset. Keep a fixed
        // 6 A Inorm trip envelope instead of falsely tripping at a 1 A command cap.
        m.config_.current_lim_margin=6.0f-m.config_.current_lim;
        if(phase<=2)m.config_.calibration_current=value==0.0f?4.0f:value;
        if(phase==1)m.config_.resistance_calib_max_voltage=cal_voltage;
        if(phase==6) {
            bench_saved_direction=e.config_.direction;bench_saved_lockin=a.config_.general_lockin;
            bench_open_loop_configured=true;
            // Native LOCKIN_SPIN checks nonzero direction, but drives electrical
            // phase directly. This convention is NOT an encoder calibration.
            e.config_.direction=1;
            auto& lockin=a.config_.general_lockin;
            lockin.current=open_current;lockin.ramp_time=0.5f;lockin.ramp_distance=0.0f;
            lockin.accel=10.0f;lockin.vel=value;lockin.finish_distance=0.0f;
            lockin.finish_on_vel=lockin.finish_on_distance=lockin.finish_on_enc_idx=false;
        }
        m.config_.torque_lim=m.config_.current_lim*0.04f;
        c.config_.control_mode=phase==4?Controller::CONTROL_MODE_VELOCITY_CONTROL:phase==5?Controller::CONTROL_MODE_POSITION_CONTROL:Controller::CONTROL_MODE_TORQUE_CONTROL;
        c.config_.input_mode=phase==4?Controller::INPUT_MODE_VEL_RAMP:Controller::INPUT_MODE_PASSTHROUGH;
        c.config_.vel_limit=phase<=2?0.5f:0.3f; // Match the calibration guard after prior closed-loop runs.
        c.reset();c.input_torque_=c.input_vel_=0.0f;c.input_pos_=p;c.input_pos_updated();
        bench_target_applied=false;bench_adc_errors=rehab_board_phase_adc_word(3);bench_gate_generation=board_access::gate_generation();
        a.config_.enable_watchdog=true;a.watchdog_feed();
        a.requested_state_=static_cast<Axis::AxisState>(bench_session.target_state());
        bench_write_status(w);return Result::Ok;
    }
    return Result::Inhibited;
}
void bench_connection_lost(void*,unsigned port,uint32_t session) {
    CRITICAL_SECTION(){if(bench_session.active() && bench_session.owns(port,session))bench_fail(BenchReason::Disconnected);}
}
