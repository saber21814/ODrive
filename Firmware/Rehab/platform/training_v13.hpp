// Private V1.3 application adapter, included inside runtime_v13's namespace.
// Engine/Service are safety-task owned. ISR-visible snapshots and handoffs use
// short interrupt exclusion; no SPI, protocol parsing or engine update in ISR.
Engine engine;
Feedback feedback;
Status published;
Target target;
uint32_t owner_session=0,owner_epoch=1,native_writers=0,native_rejected=0;
unsigned owner_port=3;
uint32_t owner_heartbeat_us=0;
bool native_clear_requested=false,training_modes_set=false;
uint32_t mutation_primask=0;
bool mutation_locked=false;
Result begin_mutation(void*,uint16_t command) {
    // Queries, emergency stop, and clear use no broad interrupt exclusion.
    // Clear has its own two-phase fault-generation check around native clearing.
    switch(static_cast<wire::Command>(command)) {
        case wire::Command::Status:case wire::Command::GetConfig:
        case wire::Command::Statistics:case wire::Command::Diagnostics:
        case wire::Command::GetLog:case wire::Command::Stop:
        case wire::Command::ClearFaults:return Result::Ok;
        default:break;
    }
    mutation_primask=__get_PRIMASK();__disable_irq();
    if(dispatching_command && (dispatch_fault_generation!=fault_generation ||
       dispatch_epoch!=owner_epoch || dispatch_port>=3 ||
       dispatch_transport_generation!=transport_generation[dispatch_port] ||
       (disconnect_bits&(1u<<dispatch_port)))) {
        __set_PRIMASK(mutation_primask);return Result::Stale;
    }
    mutation_locked=true;return Result::Ok;
}
void end_mutation(void*) {
    if(mutation_locked){mutation_locked=false;__set_PRIMASK(mutation_primask);}
}
Controller::ControlMode saved_control_mode=Controller::CONTROL_MODE_POSITION_CONTROL;
Controller::InputMode saved_input_mode=Controller::INPUT_MODE_PASSTHROUGH;
struct Limits {
    bool valid=false;
    float metres_per_encoder_turn=0,zero_turns=0,min_length=0,max_length=0,max_speed=0,max_torque=0;
    uint32_t target_us=0,sensor_us=0;
} limits;
struct LogEntry {uint32_t ms,code,value;};
LogEntry logs[64]{};
unsigned log_read=0,log_write=0,log_count=0;
uint32_t log_dropped=0;
void log(uint32_t code,uint32_t value) {
    if(log_count==64){log_read=(log_read+1)%64;--log_count;++log_dropped;}
    logs[log_write]={HAL_GetTick(),code,value};log_write=(log_write+1)%64;++log_count;
}
bool read_log(void*,uint32_t& ts,uint32_t& code,uint32_t& value) {
    if(!log_count)return false;
    const auto entry=logs[log_read];log_read=(log_read+1)%64;--log_count;
    ts=entry.ms;code=entry.code;value=entry.value;return true;
}
Feedback capture() {
    Feedback f;
    CRITICAL_SECTION(){
        f=feedback;
        f.idle=axes[0].current_state_==Axis::AXIS_STATE_IDLE;
        f.armed=motors[0].is_armed_;f.native_fault=odrv.any_error();f.stop=stop_asserted();
    }
    return f;
}
bool native_training_compatible() {
    const auto& a=axes[0];
    return !a.config_.enable_step_dir && !a.step_dir_active_ && !a.config_.enable_sensorless_mode &&
        a.encoder_.config_.mode==Encoder::MODE_INCREMENTAL &&
        a.motor_.config_.motor_type==Motor::MOTOR_TYPE_HIGH_CURRENT &&
        a.controller_.config_.load_encoder_axis==0 &&
        !a.controller_.config_.anticogging.calib_anticogging &&
        !(a.controller_.anticogging_valid_ && a.controller_.config_.anticogging.anticogging_enabled);
}
bool training_ready() {
    return owner_session && hardware_ready() && native_training_compatible() &&
        motors[0].is_calibrated_ && axes[0].encoder_.is_ready_ &&
        std::isfinite(motors[0].config_.torque_constant) && motors[0].config_.torque_constant>0;
}
// Called with interrupts excluded, only while this adapter owns the controller.
void clear_training_commands() {
    auto& c=axes[0].controller_;
    c.input_torque_=0;c.input_vel_=0;c.reset();c.torque_output_=0;
    const auto pos=axes[0].encoder_.pos_estimate_.any();
    // No stale position target is handed back, even with unavailable feedback.
    const float aligned=pos.has_value() && std::isfinite(*pos)?*pos:0.0f;
    c.set_input_pos_and_steps(aligned);c.input_pos_updated();c.pos_setpoint_=aligned;
}
Result take_control(void*,uint32_t session,const Feedback&) {
    CRITICAL_SECTION(){
        if(owner_session || native_writers || !quiescent_axis())return Result::Busy;
        if(dispatching_command && dispatch_fault_generation!=fault_generation)return Result::Stale;
        if(!native_training_compatible())return Result::InvalidConfig;
        saved_control_mode=axes[0].controller_.config_.control_mode;
        saved_input_mode=axes[0].controller_.config_.input_mode;
        training_modes_set=false;target={};
        __atomic_store_n(&owner_session,session,__ATOMIC_RELEASE);++owner_epoch;
        owner_heartbeat_us=now_us();command_boundary();
    }
    const Result result=engine.acquire(session,capture());
    if(result!=Result::Ok){
        CRITICAL_SECTION(){owner_session=0;++owner_epoch;command_boundary();}
        return result;
    }
    engine.set_epoch(owner_epoch);
    if(latched)engine.trip(latched);
    return Result::Ok;
}
Result release_control(void*,uint32_t session,const Feedback&) {
    // Retain a faulted lease until explicit ClearFaults, then Release. This also
    // prevents disconnect/STOP recovery from silently handing control back.
    CRITICAL_SECTION(){
        if(!session || session!=owner_session)return Result::NotOwner;
        if(!quiescent_axis())return Result::Busy;
        if(latched || engine.status().faults)return Result::Faulted;
        if(dispatching_command && dispatch_fault_generation!=fault_generation)return Result::Stale;
        // Engine release only publishes a disabled target and increments its
        // epoch. Commit it with the platform lease in the same short exclusion.
        // An ISR before entry is checked above; one after commit sees native idle.
        const Result result=engine.release(session,capture());
        if(result!=Result::Ok)return result;
        clear_training_commands();
        axes[0].controller_.config_.control_mode=saved_control_mode;
        axes[0].controller_.config_.input_mode=saved_input_mode;
        training_modes_set=false;target={};owner_port=3;
        __atomic_store_n(&owner_session,0,__ATOMIC_RELEASE);++owner_epoch;command_boundary();
        engine.set_epoch(owner_epoch);
    }
    return Result::Ok;
}
Result clear_control_faults(void*,uint32_t session) {
    uint32_t generation;
    CRITICAL_SECTION(){
        if(session!=owner_session || !session)return Result::NotOwner;
        if(!quiescent_axis() || stop_asserted() || !board_access::gate_ready())return Result::Inhibited;
        generation=fault_generation;
        if(dispatching_command && generation!=dispatch_fault_generation)return Result::Stale;
    }
    odrv.clear_errors();
    const auto f=capture();
    if(f.native_fault || f.stop || f.armed)return Result::Inhibited;
    engine.update(now_us(),f,latest_force);
    const Result result=engine.clear_faults(session);
    if(result!=Result::Ok)return result;
    bool cleared=false;
    CRITICAL_SECTION(){
        cleared=generation==fault_generation && !stop_asserted() && !motors[0].is_armed_;
        if(cleared){
            latched=0;target={};clear_training_commands();
            owner_epoch=engine.status().epoch;owner_heartbeat_us=now_us();command_boundary();
        }
    }
    if(!cleared){engine.trip(latched?latched:uint32_t(FaultPlatform));return Result::Inhibited;}
    return Result::Ok;
}
bool training_target_valid() {
    return target.enable && training_modes_set && target.epoch==owner_epoch && limits.valid &&
        uint32_t(now_us()-target.created_us)<=limits.target_us;
}
void training_update(uint32_t us) {
    const auto f=capture();
    if(native_clear_requested && !owner_session){
        native_clear_requested=false;engine.clear_unowned_faults(f);
    }
    if(!owner_session)return; // No application faults or writes from native activity.
    if(latched)engine.trip(latched);
    if(uint32_t(us-owner_heartbeat_us)>=engine.config().heartbeat_timeout_ms*1000u)engine.trip(FaultHeartbeat);
    engine.update(us,f,latest_force);
    if(engine.status().target.enable && latest_force.valid){
        const auto& c=engine.config();
        const float force=(float(latest_force.raw)-c.force_offset_counts)*c.force_scale_n_per_count;
        if(!std::isfinite(force) || std::fabs(force)>c.max_force_n)engine.trip(FaultForce);
    }
    if(engine.status().faults)trip(engine.status().faults,false);
}
void training_publish() {
    const auto& cfg=engine.config();
    Limits l;
    l.valid=engine.status().config_valid;
    if(l.valid){
        l.metres_per_encoder_turn=cfg.direction*6.28318530718f*cfg.radius_m /
            cfg.motor_turns_per_drum_turn / cfg.encoder_turns_per_motor_turn;
        l.zero_turns=cfg.zero_turns;l.min_length=cfg.min_length_m;l.max_length=cfg.max_length_m;
        l.max_speed=cfg.max_speed_m_s;l.max_torque=cfg.max_torque_nm;
        l.target_us=cfg.target_timeout_ms*1000u;l.sensor_us=cfg.sensor_timeout_ms*1000u;
    }
    if(owner_session && engine.status().faults)trip(engine.status().faults,false);
    CRITICAL_SECTION(){
        published=engine.status();limits=l;target=published.target;
        if(!owner_session)return;
        if(latched){target={};clear_training_commands();return;}
        if(target.enable){
            if(!training_ready()){trip(FaultNative,false);target={};return;}
            // Mode changes happen once per ownership, in task context, while
            // disarmed. Enable alone never reaches this target-enabled branch.
            if(!training_modes_set){
                if(!quiescent_axis()){trip(FaultPlatform,false);target={};return;}
                clear_training_commands();
                axes[0].controller_.config_.control_mode=Controller::CONTROL_MODE_TORQUE_CONTROL;
                axes[0].controller_.config_.input_mode=Controller::INPUT_MODE_PASSTHROUGH;
                training_modes_set=true;
            }
            if(hardware_ready() && axes[0].current_state_==Axis::AXIS_STATE_IDLE &&
               axes[0].requested_state_==Axis::AXIS_STATE_UNDEFINED)
                axes[0].requested_state_=Axis::AXIS_STATE_CLOSED_LOOP_CONTROL;
        } else {
            clear_training_commands();
            if(axes[0].current_state_!=Axis::AXIS_STATE_IDLE || motors[0].is_armed_ ||
               axes[0].requested_state_!=Axis::AXIS_STATE_UNDEFINED)
                axes[0].requested_state_=Axis::AXIS_STATE_IDLE;
        }
    }
}
void training_control() {
    auto& a=axes[0];Feedback f;
    f.timestamp_us=now_us();f.position_turns=a.encoder_.pos_estimate_.any().value_or(NAN);
    f.velocity_turns_s=a.encoder_.vel_estimate_.any().value_or(NAN);
    f.torque_nm=a.controller_.torque_output_.any().value_or(0.0f);
    f.current_a=a.motor_.current_control_.Iq_measured_;f.vbus_v=vbus_voltage;
    f.idle=a.current_state_==Axis::AXIS_STATE_IDLE;f.armed=a.motor_.is_armed_;
    f.native_fault=odrv.any_error();f.stop=stop_asserted();feedback=f;
    if(!owner_session || !target.enable)return;
    const float length=(f.position_turns-limits.zero_turns)*limits.metres_per_encoder_turn;
    const float speed=f.velocity_turns_s*limits.metres_per_encoder_turn;
    if(!training_target_valid())trip(FaultTargetExpired,false);
    else if(!std::isfinite(length) || !std::isfinite(speed) || !std::isfinite(target.torque_nm))trip(FaultNumeric,false);
    else if(length<limits.min_length || length>limits.max_length)trip(FaultLength,false);
    else if(std::fabs(speed)>limits.max_speed)trip(FaultSpeed,false);
    else if(f.native_fault)trip(FaultNative,false);
    else if(!latest_force.valid || uint32_t(f.timestamp_us-latest_force.timestamp_us)>limits.sensor_us)trip(FaultSensor,false);
    if(!latched && hardware_ready()){
        a.controller_.input_torque_=std::max(-limits.max_torque,std::min(limits.max_torque,target.torque_nm));
        a.watchdog_feed();
    }
}
void training_post_control() {
    if(!owner_session)return;
    auto& c=axes[0].controller_;auto& output=c.torque_output_;const auto actual=output.present();
    if(!target.enable || latched){c.input_torque_=0;c.input_vel_=0;output=0;return;}
    if(!actual.has_value() || !std::isfinite(*actual)){trip(FaultNative,false);output=0;return;}
    const float bound=std::min(limits.max_torque,std::fabs(target.torque_nm));
    output=(*actual)*target.torque_nm>=0?std::copysign(std::min(std::fabs(*actual),bound),target.torque_nm):0.0f;
}
