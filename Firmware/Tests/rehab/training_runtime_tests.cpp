#define TRAINING_FIXTURE_ONLY
#include "native_runtime_tests.cpp"

static uint32_t sequence=1,session=0;
static rw::Frame request(rw::Command command,unsigned port=0,uint32_t other_session=0,
                         const uint8_t* payload=nullptr,uint16_t size=0) {
    rw::Frame frame;frame.command=uint16_t(command);frame.session=other_session?other_session:session;
    frame.sequence=++sequence;frame.length=size;if(size)std::memcpy(frame.payload,payload,size);
    uint8_t bytes[rw::kMaxFrame];auto n=rw::encode(frame,bytes,sizeof(bytes));
    rp::receive_bytes(port,bytes,n);
    // Real runtime bounds parsing to 64 bytes/transport/tick.
    for(unsigned i=0;i<6;++i)rp::service_transports();
    auto out=replies(port);CHECK(out.size()==1);return out[0];
}
static void boot() {
    reset_runtime();axes[0].step_dir_active_=false;rp::latest_force={};
    rp::early_init();idle_native();rp::control_hook();sequence=1;
    ++mock_rng.DR;submit(0,rw::Command::Hello);auto out=replies(0);CHECK(out.size()==1);session=out[0].session;
}
static void step(unsigned ms=1,bool valid=true) {
    mock_now_ms+=ms;rp::control_hook();rp::latest_force.valid=valid;
    rp::latest_force.timestamp_us=rp::now_us();++rp::latest_force.sequence;
    rp::training_update(rp::now_us());rp::training_publish();
}
static void configure() {
    rehab::Config config;
    config.radius_m=.02f;config.motor_turns_per_drum_turn=2;config.efficiency=.8f;
    config.force_scale_n_per_count=.001f;config.min_length_m=-2;config.max_length_m=2;
    config.max_speed_m_s=1;config.max_force_n=200;config.max_torque_nm=2;config.torque_slew_nm_s=2;
    config.concentric_force_n=20;config.eccentric_force_n=10;config.speed_limit_m_s=.4f;
    // Exercise the actual wire config IDs and atomic commit, not a bypass.
    for(unsigned first=0;first<rw::config_field_count();first+=8){
        uint8_t payload[64]{};unsigned count=std::min(8u,unsigned(rw::config_field_count())-first);
        for(unsigned i=0;i<count;++i){uint16_t type;uint32_t bits;
            CHECK(rw::get_config_field(config,first+i,type,bits));
            rw::put16(payload+8*i,uint16_t(first+i));rw::put16(payload+8*i+2,type);rw::put32(payload+8*i+4,bits);
        }
        CHECK(result_code(request(rw::Command::SetConfig,0,0,payload,uint16_t(count*8)))==0);
    }
    CHECK(result_code(request(rw::Command::CommitConfig))==0);
    rp::latest_force.raw=10000;step();
}
static void acquire_configured() {
    boot();CHECK(result_code(request(rw::Command::Acquire))==0);configure();
}
static void start_training() {
    CHECK(result_code(request(rw::Command::Enable))==0);
    CHECK(!rp::target.enable && !motors[0].is_armed_ && axes[0].requested_state_==Axis::AXIS_STATE_UNDEFINED);
    uint8_t mode[4]{};CHECK(result_code(request(rw::Command::Start,0,0,mode,4))==0);
    CHECK(rp::target.enable && rp::training_modes_set && rp::motor_may_arm());
    CHECK(axes[0].requested_state_==Axis::AXIS_STATE_CLOSED_LOOP_CONTROL);
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;axes[0].current_state_=Axis::AXIS_STATE_CLOSED_LOOP_CONTROL;
    motors[0].is_armed_=true;step();rp::control_hook();
}
static void native_isolation() {
    boot();
    for(auto mode:{Controller::CONTROL_MODE_TORQUE_CONTROL,Controller::CONTROL_MODE_VELOCITY_CONTROL,Controller::CONTROL_MODE_POSITION_CONTROL}){
        controllers[0].config_.control_mode=mode;controllers[0].input_torque_=.7f;controllers[0].input_vel_=3;
        controllers[0].torque_output_=.4f;motors[0].is_armed_=true;
        step(5000,false);rp::post_controller_hook();rp::fast_guard();rp::disconnected(2);worker_once();
        CHECK(controllers[0].config_.control_mode==mode && controllers[0].input_torque_==.7f && controllers[0].input_vel_==3);
        CHECK(controllers[0].torque_output_.present().value()==.4f && motors[0].is_armed_);
        CHECK(!rp::latched && axes[0].watchdog_feeds==0 && !rp::engine.status().faults);
    }
    idle_native();
    for(unsigned state:{4u,7u,8u,9u})CHECK(rp::axis_request_allowed(state));
    Property<float> native_parameter{&motors[0].config_.current_lim};native_parameter.exchange(3.2f);
    CHECK(motors[0].config_.current_lim==3.2f);
    CHECK(result_code(request(rw::Command::CommitConfig))==uint32_t(Result::NotOwner));
    for(unsigned cmd=22;cmd<=26;++cmd)CHECK(result_code(request(static_cast<rw::Command>(cmd)))==uint32_t(Result::Unsupported));
    CHECK(request(rw::Command::Status).length==64 && request(rw::Command::Diagnostics).length==256);
}
static void admission_and_exit() {
    boot();
    motors[0].is_armed_=true;CHECK(result_code(request(rw::Command::Acquire))==uint32_t(Result::Busy));idle_native();
    axes[0].requested_state_=Axis::AXIS_STATE_MOTOR_CALIBRATION;
    CHECK(result_code(request(rw::Command::Acquire))==uint32_t(Result::Busy));idle_native();
    axes[0].task_chain_[2]=Axis::AXIS_STATE_MOTOR_CALIBRATION;axes[0].task_chain_[1]=Axis::AXIS_STATE_IDLE;
    CHECK(result_code(request(rw::Command::Acquire))==uint32_t(Result::Busy));idle_native();
    {rehab::NativeWriteGuard admitted;CHECK(bool(admitted));CHECK(result_code(request(rw::Command::Acquire))==uint32_t(Result::Busy));}
    axes[0].config_.enable_step_dir=true;CHECK(result_code(request(rw::Command::Acquire))!=0);axes[0].config_.enable_step_dir=false;
    axes[0].config_.enable_sensorless_mode=true;CHECK(result_code(request(rw::Command::Acquire))!=0);axes[0].config_.enable_sensorless_mode=false;
    axes[0].step_dir_active_=true;CHECK(result_code(request(rw::Command::Acquire))!=0);axes[0].step_dir_active_=false;
    controllers[0].anticogging_valid_=controllers[0].config_.anticogging.anticogging_enabled=true;
    CHECK(result_code(request(rw::Command::Acquire))!=0);controllers[0].anticogging_valid_=false;
    encoders[0].config_.mode=Encoder::MODE_HALL;CHECK(result_code(request(rw::Command::Acquire))!=0);encoders[0].config_.mode=Encoder::MODE_INCREMENTAL;
    controllers[0].config_.control_mode=Controller::CONTROL_MODE_VELOCITY_CONTROL;
    controllers[0].config_.input_mode=Controller::INPUT_MODE_VEL_RAMP;
    CHECK(result_code(request(rw::Command::Acquire))==0);
    CHECK(!rehab::native_input_allowed() && !rp::target.enable && !rp::motor_may_arm());
    Property<float> native_parameter{&motors[0].config_.current_lim};float old=motors[0].config_.current_lim;
    native_parameter.exchange(200);CHECK(motors[0].config_.current_lim==old && rp::native_rejected);
    CHECK(result_code(request(rw::Command::Enable))!=0);configure();
    motors[0].is_calibrated_=false;CHECK(result_code(request(rw::Command::Enable))==uint32_t(Result::Inhibited));motors[0].is_calibrated_=true;
    encoders[0].is_ready_=false;CHECK(result_code(request(rw::Command::Enable))==uint32_t(Result::Inhibited));encoders[0].is_ready_=true;
    start_training();CHECK(controllers[0].config_.control_mode==Controller::CONTROL_MODE_TORQUE_CONTROL);
    CHECK(controllers[0].config_.input_mode==Controller::INPUT_MODE_PASSTHROUGH && axes[0].watchdog_feeds>0);
    CHECK(result_code(request(rw::Command::Release))==uint32_t(Result::Busy));
    CHECK(result_code(request(rw::Command::Finish))==0);
    for(unsigned i=0;i<15;++i)step(); // Slew to zero, request actual native idle.
    CHECK(!rp::target.enable && axes[0].requested_state_==Axis::AXIS_STATE_IDLE);
    idle_native();step();CHECK(rp::engine.status().state==rehab::State::Standby);
    encoders[0].pos_estimate_=1.25f;controllers[0].vel_integrator_torque_=3;
    CHECK(result_code(request(rw::Command::Release))==0);
    CHECK(!rp::owner_session && !motors[0].is_armed_ && rehab::native_input_allowed());
    CHECK(controllers[0].config_.control_mode==Controller::CONTROL_MODE_VELOCITY_CONTROL && controllers[0].config_.input_mode==Controller::INPUT_MODE_VEL_RAMP);
    CHECK(controllers[0].input_torque_==0 && controllers[0].input_vel_==0 && controllers[0].vel_integrator_torque_==0 && controllers[0].input_pos_==1.25f);
    CHECK(axes[0].requested_state_==Axis::AXIS_STATE_UNDEFINED);
}
static void faults_and_sessions() {
    acquire_configured();start_training();
    rp::disconnected(2);rp::service_transports();CHECK(motors[0].is_armed_ && !rp::latched);
    rp::disconnected(0);CHECK(!motors[0].is_armed_ && rp::owner_session==session);
    rp::service_transports();step();CHECK(rp::engine.status().faults & rehab::FaultHeartbeat);
    idle_native();++mock_rng.DR;submit(0,rw::Command::Hello);auto hello=replies(0);CHECK(hello.size()==1);
    uint32_t old=session;session=hello[0].session;CHECK(session!=old);
    CHECK(result_code(request(rw::Command::Enable,0,old))!=0);
    CHECK(result_code(request(rw::Command::ClearFaults))==0);CHECK(!rp::owner_session && !rp::latched && !motors[0].is_armed_);
    acquire_configured();start_training();step(300);
    CHECK(!motors[0].is_armed_ && rp::owner_session && (rp::latched & rehab::FaultHeartbeat));
    idle_native();CHECK(result_code(request(rw::Command::Release))==uint32_t(Result::Faulted));
    CHECK(result_code(request(rw::Command::ClearFaults))==0);
    CHECK(rp::owner_session && !rp::target.enable && !motors[0].is_armed_);
    CHECK(result_code(request(rw::Command::Release))==0);
    acquire_configured();start_training();
    mock_gpio_c.IDR=0;stop_callback(nullptr);CHECK(!motors[0].is_armed_ && rp::owner_session);
    idle_native();CHECK(result_code(request(rw::Command::ClearFaults))!=0);
    mock_gpio_c.IDR=GPIO_PIN_4;gate_ready=true;step();CHECK(!rp::motor_may_arm());
    CHECK(result_code(request(rw::Command::ClearFaults))==0);CHECK(!rp::target.enable && !motors[0].is_armed_);
    // Re-trigger during native clear; generation guard must keep the latch.
    rp::trip(rehab::FaultStop);idle_native();step();
    clear_injection=[](){rp::physical_stop(true);};
    CHECK(result_code(request(rw::Command::ClearFaults))!=0);CHECK(rp::latched && rp::owner_session);
    clear_injection=nullptr;
}
static void old_generation() {
    boot();rw::PendingCommand queued{};queued.port=0;queued.frame.command=uint16_t(rw::Command::Enable);
    queued.frame.session=session;queued.frame.sequence=100;queued.received_us=rp::now_us();
    queued.epoch=rp::owner_epoch;queued.fault_generation=rp::fault_generation;
    CHECK(result_code(request(rw::Command::Acquire))==0);
    CHECK(rp::commands.push(queued));rp::service_transports();auto out=replies(0);
    CHECK(out.size()==1 && result_code(out[0])==uint32_t(Result::Stale));
    // Cached duplicate Acquire remains idempotent after the ownership boundary.
    submit(0,rw::Command::Acquire,session,sequence);out=replies(0);
    CHECK(out.size()==1 && result_code(out[0])==0 && rp::owner_session==session);
    // Explicit native IDLE remains available despite other writes being denied.
    Property<Axis::AxisState> state{&axes[0].requested_state_};state.exchange(Axis::AXIS_STATE_IDLE);
    CHECK(rp::latched && !motors[0].is_armed_ && rp::owner_session);
}
static void mutation_race_and_replay() {
    acquire_configured();
    CHECK(result_code(request(rw::Command::Heartbeat))==0);
    const auto beat=rp::owner_heartbeat_us;const auto beat_sequence=sequence;
    mock_now_ms+=100;submit(0,rw::Command::Heartbeat,session,beat_sequence);
    auto out=replies(0);CHECK(out.size()==1 && result_code(out[0])==0);
    CHECK(rp::owner_heartbeat_us==beat); // A duplicate never extends the lease.
    step(151);CHECK(rp::latched & rehab::FaultHeartbeat);
    acquire_configured();
    rw::Frame in,response;in.command=uint16_t(rw::Command::Enable);in.session=session;in.sequence=++sequence;
    rp::dispatching_command=true;rp::dispatch_port=0;rp::dispatch_epoch=rp::owner_epoch;
    rp::dispatch_fault_generation=rp::fault_generation;rp::dispatch_transport_generation=rp::transport_generation[0];
    CriticalSectionContext::before_enter=[](){
        CriticalSectionContext::before_enter=nullptr;rp::trip(rehab::FaultStop);
    };
    // Invoke Service directly to inject between queue validation and mutation.
    rp::service.handle(0,in,response,rp::now_us(),rp::feedback);
    rp::dispatching_command=false;
    CHECK(result_code(response)==uint32_t(Result::Stale));
    CHECK(!rp::target.enable && rp::owner_session==session && !mock_primask);
    // A completed Finish cannot be confused with an external STOP fault.
    acquire_configured();start_training();
    CHECK(result_code(request(rw::Command::Pause))==0);
    for(unsigned i=0;i<15;++i)step();
    idle_native();step();
    CHECK(rp::engine.status().state==rehab::State::Paused && !rp::target.enable);
    CHECK(result_code(request(rw::Command::Resume))==0 && rp::target.enable);
    rp::trip(rehab::FaultStop);idle_native();step();
    CHECK(result_code(request(rw::Command::ClearFaults))==0);
    CHECK(result_code(request(rw::Command::Release))==0 && !rp::target.enable);
}
static void observers_and_deadlines() {
    acquire_configured();++mock_rng.DR;submit(2,rw::Command::Hello);auto hello=replies(2);CHECK(hello.size()==1);
    auto observer=hello[0].session;
    uint8_t value[8]{};rw::put32(value+4,0x3f800000);
    CHECK(result_code(request(rw::Command::SetConfig,2,observer,value,8))==uint32_t(Result::NotOwner));
    CHECK(result_code(request(rw::Command::CommitConfig,2,observer))==uint32_t(Result::NotOwner));
    CHECK(result_code(request(rw::Command::Enable,2,observer))==uint32_t(Result::NotOwner));
    uint8_t mode[4]{};CHECK(result_code(request(rw::Command::Start,2,observer,mode,4))==uint32_t(Result::NotOwner));
    auto control=controllers[0].config_.control_mode;auto input=controllers[0].config_.input_mode;
    CHECK(result_code(request(rw::Command::Enable))==0);
    CHECK(controllers[0].config_.control_mode==control && controllers[0].config_.input_mode==input);
    CHECK(result_code(request(rw::Command::Start,0,0,mode,4))==0);
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;axes[0].current_state_=Axis::AXIS_STATE_CLOSED_LOOP_CONTROL;motors[0].is_armed_=true;
    mock_now_ms+=21;rp::fast_guard();
    CHECK(!motors[0].is_armed_ && (rp::latched & rehab::FaultTargetExpired) && rp::owner_session);
    acquire_configured();start_training();rp::latest_force.valid=false;rp::control_hook();
    CHECK(!motors[0].is_armed_ && (rp::latched & rehab::FaultSensor) && rp::owner_session);
    acquire_configured();auto config=rp::engine.config();
    config.radius_m=1e10f;config.motor_turns_per_drum_turn=1e30f;config.encoder_turns_per_motor_turn=1e10f;
    CHECK(rp::engine.configure(config)==Result::Ok);rp::training_publish();
    CHECK(std::isfinite(rp::limits.metres_per_encoder_turn) && rp::limits.metres_per_encoder_turn>0);
}
int main(){try{
    test_preserve_parameters_and_boot();native_isolation();admission_and_exit();faults_and_sessions();old_generation();mutation_race_and_replay();observers_and_deadlines();
    std::printf("PASS: %u V1.3 training/native integration checks\n",checks);
}catch(const TestFailure&){return 1;}return 0;}
