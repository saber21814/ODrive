// Actual extension/service/ingress with native state carriers, no motor simulation.
#define main application_runtime_main
#include "runtime_tests.cpp"
#undef main

static void idle_native() {
    axes[0].task_chain_.fill(Axis::AXIS_STATE_UNDEFINED);
    axes[0].current_state_=Axis::AXIS_STATE_IDLE;
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    motors[0].is_armed_=false;
}
static void native_fixture() {
    reset_runtime();axes[0].error_=0;
    CHECK(rp::native_board_control);
    rp::early_init();idle_native();
}
static void test_preserve_parameters_and_boot() {
    reset_runtime();axes[0].error_=0;
    auto& m=motors[0];auto& e=encoders[0];auto& a=axes[0];
    m.config_.pole_pairs=11;m.config_.current_lim=1.3f;m.config_.current_lim_margin=8;
    m.config_.I_bus_hard_min=-INFINITY;m.config_.I_bus_hard_max=INFINITY;
    m.config_.calibration_current=0.6f;m.config_.resistance_calib_max_voltage=1.2f;
    m.config_.phase_resistance=0.35f;m.config_.phase_inductance=0.0003f;
    e.config_.cpr=10000;e.config_.direction=-1;e.config_.use_index=true;
    odrv.config_.dc_bus_undervoltage_trip_level=8;odrv.config_.dc_bus_overvoltage_trip_level=0;
    odrv.config_.dc_max_positive_current=INFINITY;odrv.config_.dc_max_negative_current=-0.01f;
    odrv.config_.enable_brake_resistor=true;odrv.config_.brake_resistance=2.5f;
    a.config_.startup_motor_calibration=a.config_.startup_encoder_index_search=true;
    a.config_.startup_encoder_offset_calibration=a.config_.startup_closed_loop_control=a.config_.startup_homing=true;
    odrv.user_config_loaded_=1;
    rp::early_init();
    CHECK(a.requested_state_==Axis::AXIS_STATE_IDLE && !m.is_armed_);
    CHECK(!a.config_.startup_motor_calibration && !a.config_.startup_encoder_index_search &&
          !a.config_.startup_encoder_offset_calibration && !a.config_.startup_closed_loop_control && !a.config_.startup_homing);
    CHECK(m.config_.pole_pairs==11 && m.config_.current_lim==1.3f && m.config_.current_lim_margin==8);
    CHECK(m.config_.I_bus_hard_min==-INFINITY && m.config_.I_bus_hard_max==INFINITY);
    CHECK(m.config_.calibration_current==0.6f && m.config_.resistance_calib_max_voltage==1.2f);
    CHECK(m.config_.phase_resistance==0.35f && m.config_.phase_inductance==0.0003f);
    CHECK(e.config_.cpr==10000 && e.config_.direction==-1 && e.config_.use_index);
    CHECK(odrv.config_.dc_bus_undervoltage_trip_level==8 && odrv.config_.dc_bus_overvoltage_trip_level==0);
    CHECK(odrv.config_.dc_max_positive_current==INFINITY && odrv.config_.dc_max_negative_current==-0.01f);
    CHECK(odrv.config_.enable_brake_resistor && odrv.config_.brake_resistance==2.5f);
    CHECK(rp::network_revision==1 && rp::pending_network.ip[0]==192 && rp::pending_network.ip[2]==11 && rp::pending_network.ip[3]==2);
    CHECK(rp::pending_network.port==39400);
}
static void test_native_requests_and_writes() {
    native_fixture();
    CHECK(rp::motor_may_arm() && rp::brake_may_arm() && rehab::native_input_allowed());
    CHECK(!rehab::commissioning::may_energize); // Hardware acceptance is not forged.
    CHECK(rp::diagnostic_word(19)==0);
    Property<float> limit{&motors[0].config_.current_lim};limit.exchange(1.5f);
    CHECK(motors[0].config_.current_lim==1.5f && rp::native_writers==0);
    encoders[0].is_ready_=false;encoders[0].pos_estimate_=NAN;encoders[0].vel_estimate_=NAN;
    for(uint32_t state : {0u,1u,4u,7u,8u,9u})CHECK(rp::axis_request_allowed(state));
    // These are requests to the native axis task, NOT proof that its own motor/
    // encoder prerequisites pass. There is no extra stage/lease gate here.
    Property<Axis::AxisState> request{&axes[0].requested_state_};
    request.exchange(Axis::AXIS_STATE_MOTOR_CALIBRATION);
    CHECK(axes[0].requested_state_==Axis::AXIS_STATE_MOTOR_CALIBRATION);
    motors[0].is_armed_=true;rp::control_hook();rp::post_controller_hook();
    CHECK(motors[0].is_armed_ && axes[0].watchdog_feeds==0);
    mock_now_ms+=20000;rp::fast_guard();CHECK(motors[0].is_armed_);
    rp::disconnected(2);rp::service_transports();CHECK(motors[0].is_armed_);
    CHECK(axes[0].watchdog_feeds==0); // Native watchdog is user-configured.
    odrv.error_=2;CHECK(!rp::motor_may_arm());rp::fast_guard();
    CHECK(!motors[0].is_armed_ && rp::latched && axes[0].requested_state_==Axis::AXIS_STATE_IDLE);
}
static void test_stop_gate_and_recovery() {
    native_fixture();motors[0].is_armed_=true;
    mock_gpio_c.IDR=0;stop_callback(nullptr);
    CHECK(!motors[0].is_armed_ && rp::latched && !gate_ready);
    CHECK(!rp::motor_may_arm() && !rp::axis_request_allowed(4));
    CHECK(rp::brake_may_arm()); // Safe native CCR writes must still be possible.
    idle_native();rp::native_clear_faults();CHECK(rp::latched);
    mock_gpio_c.IDR=GPIO_PIN_4;gate_ready=true;
    CHECK(!rp::motor_may_arm());rp::native_clear_faults();
    CHECK(!rp::latched && rp::motor_may_arm() && !motors[0].is_armed_);
    CHECK(axes[0].requested_state_==Axis::AXIS_STATE_UNDEFINED);
    motors[0].is_armed_=true;gate_ready=false;rp::fast_guard();
    CHECK(!motors[0].is_armed_ && rp::latched && rp::brake_may_arm());
    gate_ready=true;CHECK(!rp::motor_may_arm());
    reset_runtime();axes[0].error_=0;mock_gpio_c.IDR=0;rp::early_init();
    CHECK(rp::latched && !rp::motor_may_arm() && rp::brake_may_arm());
}
static void test_peripheral_protocol_only() {
    native_fixture();const auto session=negotiate(2);uint32_t seq=2;
    for(unsigned cmd : {6u,8u,9u,22u,23u,24u,25u,26u}) {
        submit(2,static_cast<rw::Command>(cmd),session,seq++);
        const auto out=replies(2);CHECK(out.size()==1);
        CHECK(result_code(out[0])==uint32_t(Result::Unsupported));
        CHECK(!rp::owner_session && !motors[0].is_armed_);
    }
    CHECK(rp::take_control(nullptr,session,rp::capture())==Result::Unsupported);
    for(auto cmd : {rw::Command::Status,rw::Command::Diagnostics}) {
        submit(2,cmd,session,seq++);const auto out=replies(2);CHECK(out.size()==1);
        CHECK(result_code(out[0])==uint32_t(Result::Ok));
        if(cmd==rw::Command::Diagnostics)CHECK(out[0].length==256);
    }
    mock_now_ms=40;worker_once();CHECK(board_spi_services>0 && rp::latest_force.sequence>0);
    CHECK(std::isfinite(rp::fet_temp) && std::isfinite(rp::aux_temp));
    motors[0].is_armed_=true;submit(2,rw::Command::Stop,session,seq++);
    CHECK(!motors[0].is_armed_ && rp::latched);
}
int main() {
    const struct {const char* name;void(*run)();} cases[]={
        {"native parameters preserved, startup stays idle",test_preserve_parameters_and_boot},
        {"native writes/requests, no lease or implicit feed",test_native_requests_and_writes},
        {"physical STOP/gate interlocks and explicit recovery",test_stop_gate_and_recovery},
        {"peripheral-only protocol and retired commands",test_peripheral_protocol_only},
    };
    for(auto t:cases){try{t.run();std::printf("PASS %s\n",t.name);}catch(const TestFailure&){std::printf("FAIL %s\n",t.name);}}
    std::printf("native peripherals runtime: %u checks, %u failed; no physical motor simulation\n",checks,failures);
    return failures?1:0;
}
