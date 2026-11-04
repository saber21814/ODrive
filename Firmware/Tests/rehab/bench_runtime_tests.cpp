// Actual runtime/service/framing with explicit state/measurement carriers only.
// No native motor calibration algorithm, electrical timing or plant is simulated.
#define main legacy_runtime_main
#include "runtime_tests.cpp"
#undef main
#include <algorithm>
using BS=rehab::bench::State;
using BR=rehab::bench::Reason;
static uint32_t nonce=0, next_sequence=2;
static void bench_fixture() {
    rp::bench_quiesce(); // Restore any prior temporary open-loop configuration first.
    reset_runtime();
    rp::bench_session={};rp::bench_motor_passed=rp::bench_encoder_passed=false;
    rp::bench_target_applied=rp::bench_boot_blocked=false;
    rp::early_init();axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    axes[0].error_=0;vbus_voltage=24;rp::fet_temp=rp::aux_temp=26;
    phase_adc_words[0]=1;phase_adc_words[1]=0x87000000u;phase_adc_words[2]=phase_adc_words[3]=0;
    motors[0].dc_calib_running_since_=10;motors[0].max_dc_calib_=6.075f;
    motors[0].phase_current_rev_gain_=0.025f;
    encoders[0].pos_estimate_=0;encoders[0].vel_estimate_=0;
    nonce=negotiate(2);next_sequence=2;
}
static rw::Frame request(uint16_t cmd,uint32_t stage=0,float arg=0,unsigned port=2,uint32_t session=0,uint32_t seq=0) {
    rw::Frame f;f.command=cmd;f.session=session?session:nonce;f.sequence=seq?seq:next_sequence++;
    if(cmd==22){f.length=8;rw::put32(f.payload,stage);std::memcpy(f.payload+4,&arg,4);}
    uint8_t bytes[rw::kMaxFrame];const auto n=rw::encode(f,bytes,sizeof(bytes));
    rp::receive_bytes(port,bytes,n);rp::service_transports();auto out=replies(port);
    CHECK(out.size()==1);return out[0];
}
static rw::Frame extended_start(uint32_t phase,float argument,float third,uint32_t duration,
                                uint16_t length=16,uint32_t seq=0) {
    rw::Frame f;f.command=22;f.session=nonce;f.sequence=seq?seq:next_sequence++;
    f.length=length;rw::put32(f.payload,phase);std::memcpy(f.payload+4,&argument,4);
    std::memcpy(f.payload+8,&third,4);rw::put32(f.payload+12,duration);
    uint8_t bytes[rw::kMaxFrame];const auto n=rw::encode(f,bytes,sizeof(bytes));
    rp::receive_bytes(2,bytes,n);rp::service_transports();auto out=replies(2);
    CHECK(out.size()==1);return out[0];
}
static void accepted(const rw::Frame& f) {CHECK(result_code(f)==uint32_t(Result::Ok));CHECK(f.length==192);}
static void quiet() {motors[0].is_armed_=false;axes[0].current_state_=Axis::AXIS_STATE_IDLE;axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;}
static void enter(uint32_t phase) {
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    axes[0].current_state_=static_cast<Axis::AxisState>(phase==1?4:phase==2?7:phase==6?9:8);
    rp::fast_guard();CHECK(rp::motor_may_arm());
    motors[0].is_armed_=true;rp::control_hook();
}
static void calibrate_both() {
    accepted(request(22,1));CHECK(controllers[0].config_.vel_limit==0.5f);enter(1);
    motors[0].is_armed_=false;CHECK(rp::motor_may_arm());motors[0].is_armed_=true;
    CHECK(rp::motor_may_arm());
    motors[0].is_calibrated_=true;motors[0].config_.phase_resistance=0.054f;motors[0].config_.phase_inductance=0.000143f;
    quiet();rp::fast_guard();CHECK(rp::bench_motor_passed && rp::bench_session.state==BS::Complete);
    accepted(request(22,2));CHECK(controllers[0].config_.vel_limit==0.5f);enter(2);encoders[0].is_ready_=true;encoders[0].config_.direction=-1;
    CHECK(controllers[0].config_.enable_overspeed_error);
    quiet();rp::fast_guard();CHECK(rp::bench_encoder_passed && rp::bench_session.state==BS::Complete);
}
static void finish_at_deadline() {
    const uint32_t end=rp::bench_session.started_ms+rp::bench_session.duration();
    while(uint32_t(end-mock_now_ms)>100){mock_now_ms+=100;accepted(request(23));rp::control_hook();}
    mock_now_ms=end;rp::fast_guard();
}
static void test_boot_order_and_motion() {
    bench_fixture();CHECK(!rp::motor_may_arm() && !rp::brake_may_arm());
    CHECK(!rehab::begin_native_write() && !rehab::native_input_allowed());
    CHECK(rp::axis_request_allowed(1) && !rp::axis_request_allowed(4));
    CHECK(result_code(request(22,2))==uint32_t(Result::Inhibited));
    CHECK(result_code(request(22,4,0.1f))==uint32_t(Result::Inhibited));
    accepted(request(25));calibrate_both();
    for(unsigned stage : {3u,4u,5u}) {
        quiet();encoders[0].pos_estimate_=0;encoders[0].vel_estimate_=0;
        const float arg=stage==3?0.0f:stage==4?0.2f:0.5f;
        accepted(request(22,stage,arg));
        CHECK(controllers[0].config_.enable_overspeed_error);
        CHECK(controllers[0].config_.vel_limit==0.3f);
        CHECK(motors[0].config_.current_lim+motors[0].config_.current_lim_margin==6.0f);
        CHECK(controllers[0].input_pos_==0 && !rp::bench_target_applied);
        CHECK(!rp::axis_request_allowed(4));enter(stage);CHECK(rp::bench_target_applied);
        if(stage==5){CHECK(controllers[0].input_pos_==0.5f);encoders[0].pos_estimate_=0.5f;}
        finish_at_deadline();CHECK(rp::bench_session.state==BS::Complete);
        CHECK(!motors[0].is_armed_ && !axes[0].config_.enable_watchdog);
    }
}
static void test_feed_and_disconnect() {
    bench_fixture();accepted(request(22,1));enter(1);
    const auto other=negotiate(0);
    CHECK(result_code(request(23,0,0,0,other,2))==uint32_t(Result::NotOwner));
    mock_now_ms+=100;const auto sequence=next_sequence++;accepted(request(23,0,0,2,nonce,sequence));
    const auto fed=rp::bench_session.fed_ms;mock_now_ms+=100;
    accepted(request(23,0,0,2,nonce,sequence));CHECK(rp::bench_session.fed_ms==fed);
    mock_now_ms=fed+500;CHECK(result_code(request(23))==uint32_t(Result::Faulted));
    CHECK(!motors[0].is_armed_ && rp::bench_session.reason==BR::Heartbeat);
    CHECK(!rp::motor_may_arm() && !axes[0].config_.enable_watchdog);
    for(unsigned kind=0;kind<4;++kind) {
        bench_fixture();accepted(request(22,1));enter(1);
        if(kind==0)accepted(request(24));
        if(kind==1){mock_gpio_c.IDR=0;stop_callback(nullptr);}
        if(kind==2){++mock_rng.DR;submit(2,rw::Command::Hello,0,99);CHECK(!replies(2).empty());}
        if(kind==3)rp::disconnected(2);
        CHECK(!motors[0].is_armed_ && !rp::bench_session.active() && !rp::motor_may_arm());
        CHECK(!axes[0].config_.enable_watchdog && !rp::bench_motor_passed && !rp::bench_encoder_passed);
        CHECK(controllers[0].config_.enable_overspeed_error);
    }
}
static void test_health_faults() {
    for(unsigned kind=0;kind<9;++kind) {
        bench_fixture();calibrate_both();quiet();accepted(request(22,3));enter(3);
        BR expected=BR::None;
        switch(kind) {
            case 0:vbus_voltage=29;expected=BR::Bus;break;
            case 1:rp::fet_temp=50;expected=BR::Temperature;break;
            case 2:phase_adc_words[1]=0x80000000u;expected=BR::Adc;break;
            case 3:phase_adc_words[2]=11;expected=BR::Adc;break;
            case 4:++phase_adc_words[3];expected=BR::Adc;break;
            case 5:encoders[0].vel_estimate_=0.7f;expected=BR::Speed;break;
            case 6:encoders[0].pos_estimate_=0.26f;expected=BR::Travel;break;
            case 7:encoders[0].pos_estimate_=NAN;expected=BR::Numeric;break;
            case 8:odrv.fault=true;expected=BR::Native;break;
        }
        rp::fast_guard();CHECK(rp::bench_session.state==BS::Fault && rp::bench_session.reason==expected);
        CHECK(!motors[0].is_armed_ && !axes[0].config_.enable_watchdog && !rp::bench_motor_passed);
    }
    bench_fixture();calibrate_both();quiet();accepted(request(22,5,0.5f));enter(5);
    finish_at_deadline();CHECK(rp::bench_session.reason==BR::Position && !motors[0].is_armed_);
}
static void test_start_and_native_bypass() {
    for(unsigned kind=0;kind<8;++kind) {
        bench_fixture();
        switch(kind) {
            case 0:vbus_voltage=21;break;
            case 1:phase_adc_words[1]|=0x08000000u;break;
            case 2:motors[0].dc_calib_running_since_=0;break;
            case 3:motors[0].phase_current_rev_gain_=0.05f;break;
            case 4:motors[0].DC_calib_.phB=7;break;
            case 5:encoders[0].vel_estimate_=0.03f;rp::bench_motor_passed=true;break;
            case 6:rp::bench_boot_blocked=true;break;
            case 7:rp::aux_temp=NAN;break;
        }
        CHECK(result_code(request(22,kind==5?2:1))==uint32_t(Result::Inhibited));
        CHECK(!rp::bench_session.active() && !motors[0].is_armed_);
    }
    bench_fixture();accepted(request(22,1,1.0f,2,nonce,2));
    CHECK(motors[0].config_.calibration_current==1);
    const auto epoch=rp::bench_session.epoch,started=rp::bench_session.started_ms;
    mock_now_ms+=100;accepted(request(22,1,1.0f,2,nonce,2));
    CHECK(rp::bench_session.epoch==epoch && rp::bench_session.started_ms==started);
    next_sequence=3;enter(1);
    Property<float> current_limit{&motors[0].config_.current_lim};
    current_limit.exchange(10000);CHECK(motors[0].config_.current_lim==5);
    Property<Axis::AxisState> state{&axes[0].requested_state_};
    state.exchange(Axis::AXIS_STATE_IDLE);
    CHECK(!motors[0].is_armed_ && !rp::bench_session.active() && rp::bench_session.reason==BR::Stop);
    CHECK(!rp::motor_may_arm() && !axes[0].config_.enable_watchdog);

    bench_fixture();accepted(request(22,1));
    mock_now_ms+=400;accepted(request(23));mock_now_ms+=100;rp::fast_guard();
    CHECK(rp::bench_session.reason==BR::UnexpectedState && !rp::motor_may_arm());
    bench_fixture();accepted(request(22,1));enter(1);
    motors[0].is_calibrated_=true;motors[0].config_.phase_resistance=NAN;
    motors[0].config_.phase_inductance=0.000143f;
    quiet();rp::fast_guard();CHECK(rp::bench_session.reason==BR::Prerequisite && !rp::bench_motor_passed);
    // Config-loaded interlock originates from actual early_init, not merely a manually set flag.
    bench_fixture();odrv.user_config_loaded_=4;rp::early_init();quiet();
    CHECK(rp::bench_boot_blocked && result_code(request(22,1))==uint32_t(Result::Inhibited));
}
static void test_calibration_idle_at_deadline() {
    for(unsigned phase : {1u,2u}) {
        bench_fixture();
        if(phase==2){calibrate_both();quiet();}
        accepted(request(22,phase));enter(phase);
        // Model native success fields, so deadline ordering is the only reason
        // certification is refused. No calibration algorithm runs in this model.
        motors[0].is_calibrated_=true;motors[0].config_.phase_resistance=0.054f;
        motors[0].config_.phase_inductance=0.000143f;
        encoders[0].is_ready_=true;encoders[0].config_.direction=1;
        const auto deadline=rp::bench_session.started_ms+rp::bench_session.duration();
        while(uint32_t(deadline-mock_now_ms)>100){mock_now_ms+=100;accepted(request(23));}
        CHECK(uint32_t(deadline-rp::bench_session.fed_ms)<500);
        quiet();mock_now_ms=deadline;rp::fast_guard();
        CHECK(rp::bench_session.state==BS::Fault && rp::bench_session.reason==BR::Deadline);
        CHECK(!rp::bench_motor_passed && !rp::bench_encoder_passed);
        CHECK(!rp::motor_may_arm() && !motors[0].is_armed_ && !axes[0].config_.enable_watchdog);
    }
}
static void motor_only_prerequisite() {
    // Explicit carrier-native result; does not implement/simulate calibration.
    accepted(request(22,1));enter(1);
    motors[0].is_calibrated_=true;motors[0].config_.phase_resistance=0.054f;
    motors[0].config_.phase_inductance=0.000143f;
    quiet();rp::fast_guard();CHECK(rp::bench_motor_passed && !rp::bench_encoder_passed);
}
static void prepare_phase(unsigned phase) {
    bench_fixture();
    if(phase==6)motor_only_prerequisite();
    else if(phase>=2){calibrate_both();quiet();}
}
static void start_phase(unsigned phase) {
    if(phase==6)accepted(extended_start(6,5,0.5f,1000));
    else accepted(request(22,phase));
}
static void rotate_idle_queue() {
    // Exercise the actual std::rotate operation used by native Axis after its
    // IDLE handler returns. This is a scheduling-state carrier, not motor physics.
    auto& a=axes[0];
    CHECK(a.task_chain_[0]==Axis::AXIS_STATE_IDLE);
    std::rotate(a.task_chain_.begin(),a.task_chain_.begin()+1,a.task_chain_.end());
    a.task_chain_.back()=Axis::AXIS_STATE_UNDEFINED;
    CHECK(a.current_state_==Axis::AXIS_STATE_UNDEFINED);
}
static void test_pending_queue_transition() {
    for(unsigned phase=1;phase<=6;++phase)for(bool wrap : {false,true}) {
        prepare_phase(phase);if(wrap)mock_now_ms=0xfffffff0u;
        start_phase(phase);const auto target=axes[0].requested_state_;
        rp::fast_guard();CHECK(rp::bench_session.state==BS::Pending && !rp::motor_may_arm());
        rotate_idle_queue();mock_now_ms+=32;rp::fast_guard();rp::control_hook();
        CHECK(rp::bench_session.state==BS::Pending && !rp::bench_session.entered);
        CHECK(!rp::motor_may_arm() && !motors[0].is_armed_);
        axes[0].task_chain_[0]=target;axes[0].task_chain_[1]=Axis::AXIS_STATE_IDLE;
        axes[0].task_chain_[2]=Axis::AXIS_STATE_UNDEFINED;
        axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
        rp::fast_guard();CHECK(rp::bench_session.state==BS::Running && rp::motor_may_arm());
        CHECK(!rp::axis_request_allowed(target==4?7:4));
    }
    for(unsigned phase=1;phase<=6;++phase)for(unsigned bad=0;bad<6;++bad) {
        prepare_phase(phase);start_phase(phase);rotate_idle_queue();
        BR reason=BR::UnexpectedState;
        if(bad==0)axes[0].requested_state_=Axis::AXIS_STATE_IDLE;
        if(bad==1)axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
        if(bad==2)motors[0].is_armed_=true;
        if(bad==3){enter(phase);motors[0].is_armed_=false;axes[0].current_state_=Axis::AXIS_STATE_UNDEFINED;}
        if(bad==4){mock_now_ms+=400;accepted(request(23));mock_now_ms+=100;}
        if(bad==5){mock_now_ms+=500;reason=BR::Heartbeat;}
        rp::fast_guard();CHECK(rp::bench_session.state==BS::Fault && rp::bench_session.reason==reason);
        CHECK(!motors[0].is_armed_ && !rp::motor_may_arm());
    }
}
static void test_encoder_independent_and_voltage() {
    for(bool original : {false,true}) {
        bench_fixture();controllers[0].config_.enable_overspeed_error=original;
        accepted(request(22,1));CHECK(!controllers[0].config_.enable_overspeed_error);
        accepted(request(24));CHECK(controllers[0].config_.enable_overspeed_error==original);
    }
    for(bool over_range : {false,true}) {
        bench_fixture();encoders[0].pos_estimate_.reset();encoders[0].vel_estimate_.reset();
        encoders[0].config_.cpr=0;encoders[0].config_.direction=-1;encoders[0].is_ready_=false;
        accepted(extended_start(1,0.5f,0.5f,0,12));
        CHECK(!controllers[0].config_.enable_overspeed_error);
        CHECK(motors[0].config_.calibration_current==0.5f && motors[0].config_.resistance_calib_max_voltage==0.5f);
        enter(1);CHECK(!encoders[0].is_ready_);
        motors[0].is_calibrated_=true;motors[0].config_.phase_resistance=over_range?1.1f:1.02f;
        motors[0].config_.phase_inductance=0.000143f;quiet();rp::fast_guard();
        CHECK(rp::bench_motor_passed==!over_range);
        CHECK(rp::bench_session.state==(over_range?BS::Fault:BS::Complete));
        CHECK(controllers[0].config_.enable_overspeed_error);
    }
    bench_fixture();accepted(request(22,1,0.1f));
    CHECK(motors[0].config_.calibration_current==0.1f && motors[0].config_.resistance_calib_max_voltage==3);
    for(float voltage : {0.09f,3.01f,NAN}) {
        bench_fixture();CHECK(result_code(extended_start(1,0.5f,voltage,0,12))==uint32_t(Result::InvalidConfig));
    }
}
static void test_openloop_bounds_replay_and_exit() {
    bench_fixture();CHECK(result_code(extended_start(6,5,0.5f,1000))==uint32_t(Result::Inhibited));
    for(unsigned bad=0;bad<10;++bad) {
        prepare_phase(6);float speed=5,current=0.5f;uint32_t ms=1000;uint16_t length=16;
        switch(bad){case 0:speed=0;break;case 1:speed=20.01f;break;case 2:speed=NAN;break;
            case 3:current=0.09f;break;case 4:current=1.51f;break;case 5:current=NAN;break;
            case 6:ms=199;break;case 7:ms=5001;break;case 8:length=8;break;case 9:length=12;break;}
        CHECK(result_code(extended_start(6,speed,current,ms,length))==uint32_t(Result::InvalidConfig));
        CHECK(!rp::bench_session.active());
    }
    for(unsigned exit=0;exit<10;++exit)for(bool original_overspeed : {false,true}) {
        prepare_phase(6);encoders[0].config_.direction=-1;encoders[0].config_.cpr=0;
        controllers[0].config_.enable_overspeed_error=original_overspeed;
        encoders[0].pos_estimate_.reset();encoders[0].vel_estimate_.reset();
        encoders[0].shadow_count_=88;CHECK(!encoders[0].is_ready_);
        const auto previous_lock=axes[0].config_.general_lockin;
        const auto sequence=next_sequence++;accepted(extended_start(6,-20,1.5f,1000,16,sequence));
        const auto epoch=rp::bench_session.epoch,started=rp::bench_session.started_ms;
        mock_now_ms+=100;accepted(extended_start(6,-20,1.5f,1000,16,sequence));
        CHECK(rp::bench_session.epoch==epoch && rp::bench_session.started_ms==started);
        const auto& lock=axes[0].config_.general_lockin;
        CHECK(lock.current==1.5f && lock.vel==-20 && lock.ramp_time==0.5f && lock.accel==10);
        CHECK(!lock.finish_on_vel && !lock.finish_on_distance && !lock.finish_on_enc_idx);
        CHECK(encoders[0].config_.direction==1 && !encoders[0].is_ready_);
        CHECK(!controllers[0].config_.enable_overspeed_error);
        enter(6);controllers[0].torque_output_=0.2f;rp::post_controller_hook();
        CHECK(controllers[0].torque_output_.any().value()==0.2f);
        CHECK(encoders[0].shadow_count_==88 && !encoders[0].pos_estimate_.any().has_value());
        if(exit==0){finish_at_deadline();CHECK(rp::bench_session.state==BS::Complete);}
        if(exit==1)accepted(request(24));
        if(exit==2)rp::disconnected(2);
        if(exit==3){Property<Axis::AxisState> state{&axes[0].requested_state_};state.exchange(Axis::AXIS_STATE_IDLE);}
        if(exit==4){mock_gpio_c.IDR=0;stop_callback(nullptr);}
        if(exit==5){axes[0].current_state_=Axis::AXIS_STATE_CLOSED_LOOP_CONTROL;rp::fast_guard();}
        if(exit==6){odrv.fault=true;rp::fast_guard();}
        if(exit==7){mock_now_ms+=500;rp::fast_guard();}
        if(exit==8){quiet();rp::fast_guard();CHECK(rp::bench_session.reason==BR::UnexpectedState);}
        if(exit==9){++mock_rng.DR;submit(2,rw::Command::Hello,0,99);CHECK(!replies(2).empty());}
        CHECK(!rp::bench_session.active() && !motors[0].is_armed_ && !rp::motor_may_arm());
        CHECK(encoders[0].config_.direction==-1 && !encoders[0].is_ready_ && !rp::bench_encoder_passed);
        CHECK(controllers[0].config_.enable_overspeed_error==original_overspeed);
        const auto& restored=axes[0].config_.general_lockin;
        CHECK(restored.current==previous_lock.current && restored.vel==previous_lock.vel && restored.accel==previous_lock.accel);
        CHECK(restored.ramp_time==previous_lock.ramp_time && restored.ramp_distance==previous_lock.ramp_distance);
        CHECK(restored.finish_distance==previous_lock.finish_distance && restored.finish_on_vel==previous_lock.finish_on_vel &&
              restored.finish_on_distance==previous_lock.finish_on_distance && restored.finish_on_enc_idx==previous_lock.finish_on_enc_idx);
    }
    // Corrupt Start must not execute; the legal frame with that sequence still can.
    prepare_phase(6);rw::Frame f;f.command=22;f.session=nonce;f.sequence=next_sequence;f.length=16;
    rw::put32(f.payload,6);float speed=5,current=0.5f;
    std::memcpy(f.payload+4,&speed,4);std::memcpy(f.payload+8,&current,4);rw::put32(f.payload+12,1000);
    uint8_t bytes[rw::kMaxFrame];const auto n=rw::encode(f,bytes,sizeof(bytes));bytes[n-1]^=1;
    rp::receive_bytes(2,bytes,n);rp::service_transports();CHECK(replies(2).empty() && !rp::bench_session.active());
    bytes[n-1]^=1;rp::receive_bytes(2,bytes,n);rp::service_transports();const auto out=replies(2);
    CHECK(out.size()==1);accepted(out[0]);CHECK(rp::bench_session.stage==6);
}
int main() {
    const struct {const char* name;void(*run)();} cases[]={
        {"boot/calibration/closed phases",test_boot_order_and_motion},
        {"feed replay and disconnect/STOP",test_feed_and_disconnect},
        {"health faults and position result",test_health_faults},
        {"start checks and native-write exclusion",test_start_and_native_bypass},
        {"calibration IDLE at deadline cannot certify",test_calibration_idle_at_deadline},
        {"native queue rotation pending bridge",test_pending_queue_transition},
        {"encoder independent calibration and voltage",test_encoder_independent_and_voltage},
        {"open-loop bounds replay and terminal restoration",test_openloop_bounds_replay_and_exit},
    };
    for(auto t:cases){try{t.run();std::printf("PASS %s\n",t.name);}catch(const TestFailure&){std::printf("FAIL %s\n",t.name);}}
    std::printf("bench runtime: %u checks, %u failed; no physical motor simulation\n",checks,failures);
    return failures?1:0;
}
