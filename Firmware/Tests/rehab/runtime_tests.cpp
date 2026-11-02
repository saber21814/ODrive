// Compile this file instead of separately compiling runtime.cpp: direct inclusion
// makes its actual private state accessible to deterministic integration tests.
// No production source is replaced, copied, or edited by this test.
#include "runtime_mocks/odrive_main.h"
#include "runtime_mocks/freertos_vars.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>
#include "fibre-cpp/protocol.hpp"
#include "Rehab/platform/runtime.cpp"

GPIO_TypeDef mock_gpio_a, mock_gpio_b, mock_gpio_c;
SPI_TypeDef mock_spi3;
DMA_Stream_TypeDef mock_streams[8];
MockDwt mock_dwt;
MockCoreDebug mock_debug;
MockNvic mock_nvic;
MockRng mock_rng;
uint32_t mock_primask = 0, mock_ipsr = 0, mock_flags_cleared = 0, mock_cycle_step = 1;
Motor motors[1]; Controller controllers[1]; Encoder encoders[1]; Axis axes[1];
ODrive odrv; MockThermistor fet_thermistors[1]; MockSpiArbiter ext_spi_arbiter;
float vbus_voltage = 24;
osMessageQId usb_event_queue = reinterpret_cast<void*>(1), uart_event_queue = reinterpret_cast<void*>(2);
static uint32_t mock_now_ms = 1;
static unsigned checks = 0, failures = 0, stack_scans = 0;
static void (*stop_callback)(void*)=nullptr;
static GPIO_TypeDef* stop_port=nullptr;
static uint16_t stop_pin=0;
#if defined(REHAB_V13)
static bool gate_ready=true;
static uint32_t gate_generation=0, board_spi_services=0;
void rehab_board_physical_stop(bool edge) {
    // Board is separately tested with the actual production gate wrapper.
    if(edge || gate_ready)++gate_generation;
    gate_ready=false;
}
bool rehab_board_gate_ready() { return gate_ready; }
uint32_t rehab_board_gate_generation() { return gate_generation; }
void rehab_board_service_spi(uint32_t now) { ext_spi_arbiter.service(now);++board_spi_services; }
Stm32SpiArbiter::Diagnostics rehab_board_native_spi_diagnostics() { return {}; }
#endif
static void (*clear_injection)() = nullptr;
struct Event { osMessageQId queue; uint32_t id; };
static std::vector<Event> events;
struct EndIteration {};
struct TestFailure {};
static void check(bool ok, const char* expr, int line) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr,"FAIL runtime_tests.cpp:%d: %s\n",line,expr); throw TestFailure{}; }
}
#define CHECK(expr) check(bool(expr), #expr, __LINE__)

uint32_t HAL_GetTick() { return mock_now_ms; }
bool Stm32Gpio::config(uint32_t,uint32_t,uint32_t) { return true; }
bool Stm32Gpio::subscribe(bool,bool,void(*callback)(void*),void*) {
    stop_callback=callback;stop_port=port_;stop_pin=pin_mask_;return true;
}
osThreadId osThreadCreate(const MockThreadDefinition*,void*) { return reinterpret_cast<void*>(3); }
uint32_t xTaskGetTickCount() { return mock_now_ms; }
uint32_t uxTaskGetStackHighWaterMark(osThreadId) { ++stack_scans; return 64; }
void vTaskDelayUntil(uint32_t* tick,uint32_t delay) { *tick += delay; mock_now_ms += delay; throw EndIteration{}; }
int osMessagePut(osMessageQId queue,uint32_t event,uint32_t) { events.push_back({queue,event}); return 0; }
float rehab_board_adc_voltage(uint32_t) { return 1.65f; }
void ODrive::clear_errors() {
    // Match production ordering: native_clear_faults hook, then native history.
    rehab::platform::native_clear_faults();
    fault = false;
    if (clear_injection) clear_injection();
}

namespace rp = rehab::platform;
namespace rw = rehab::wire;
using rehab::Result;
static void reset_runtime() {
    mock_now_ms = 1; mock_primask = mock_ipsr = 0; clear_injection = nullptr; events.clear(); stack_scans = 0;
    motors[0] = Motor{}; controllers[0] = Controller{}; encoders[0] = Encoder{}; odrv = ODrive{};
    axes[0].config_ = Axis::Config{}; axes[0].task_chain_.fill(Axis::AXIS_STATE_UNDEFINED);
    axes[0].current_state_ = Axis::AXIS_STATE_IDLE; axes[0].requested_state_ = Axis::AXIS_STATE_UNDEFINED;
    axes[0].step_dir_active_ = false; axes[0].watchdog_feeds = 0;
    mock_rng = MockRng{}; mock_gpio_a.IDR = mock_gpio_b.IDR = mock_gpio_c.IDR = 0;
#if defined(REHAB_V13)
    mock_gpio_c.IDR=GPIO_PIN_4;gate_ready=true;gate_generation=board_spi_services=0;
#endif
    rp::engine.reset(); rp::feedback = rehab::Feedback{}; rp::feedback.timestamp_us = 1000; rp::feedback.vbus_v = 24;
    rp::published = rehab::Status{}; rp::target = rehab::Target{}; rp::latest_force = rehab::ForceSample{};
    rp::limits = rp::Limits{}; rp::owner_session = 0; rp::owner_epoch = 1;
    rp::native_writers = rp::native_rejected = rp::latched = rp::disconnect_bits = 0;
    rp::fault_generation = rp::native_clear_requested = 0;
    rp::rx_overflow = rp::tx_overflow = rp::command_overflow = 0;
    rp::commands={};rp::command_queue_full=rp::command_stale=0;
    rp::boundary_revision=0;for(unsigned i=0;i<3;++i)rp::rx_boundary[i]=rp::boundary_seen[i]=0;
    CriticalSectionContext::before_enter=nullptr;
    rp::dispatching_command=false;rp::dispatch_fault_generation=0;
    for(auto& generation:rp::transport_generation)generation=0;
    rp::log_read = rp::log_write = rp::log_count = rp::log_dropped = 0; rp::started = false;
    for (unsigned i = 0; i < 3; ++i) { rp::rx[i] = {}; rp::tx[i] = {}; rp::parsers[i] = {}; }
    rp::service.~Service(); new (&rp::service) rw::Service(rp::engine,rp::hooks);
}
static void worker_once() { try { rp::safety_main(nullptr); } catch (const EndIteration&) {} }
static bool saw_event(osMessageQId queue,uint32_t id) {
    for (auto e : events) if (e.queue == queue && e.id == id) return true;
    return false;
}
static uint32_t result_code(const rw::Frame& frame) { return rw::u32(frame.payload); }
static std::vector<rw::Frame> replies(unsigned port) {
    uint8_t bytes[1024]; const size_t n = rp::transmit_bytes(port,bytes,sizeof(bytes));
    std::vector<rw::Frame> frames;
    rw::Parser parser;
    parser.consume(bytes,n,mock_now_ms,[](void* p,const rw::Frame& frame) {
        static_cast<std::vector<rw::Frame>*>(p)->push_back(frame);
    },&frames);
    return frames;
}
static void send_bytes(unsigned port,rw::Command command,uint32_t session=0,uint32_t sequence=1) {
    rw::Frame frame; frame.command=uint16_t(command); frame.session=session; frame.sequence=sequence;
    uint8_t bytes[rw::kMaxFrame]; const size_t n=rw::encode(frame,bytes,sizeof(bytes));
    rp::receive_bytes(port,bytes,n);
}
static void submit(unsigned port,rw::Command command,uint32_t session=0,uint32_t sequence=1) {
    send_bytes(port,command,session,sequence);rp::service_transports();
}
static void active_target(uint32_t epoch=4) {
    rp::owner_session=11; rp::owner_epoch=epoch;
    rp::target.enable=true; rp::target.epoch=epoch; rp::target.created_us=mock_now_ms*1000;
    rp::target.torque_nm=-0.5f;
    rp::limits.valid=true; rp::limits.target_us=20000; rp::limits.sensor_us=250000;
    rp::limits.metres_per_encoder_turn=0.1f; rp::limits.zero_turns=0;
    rp::limits.min_length=-10; rp::limits.max_length=10; rp::limits.max_speed=2; rp::limits.max_torque=1;
    rp::latest_force.valid=true; rp::latest_force.timestamp_us=mock_now_ms*1000; rp::latest_force.sequence=1;
}
static void advance_engine() {
    ++mock_now_ms;
    rp::feedback.timestamp_us=mock_now_ms*1000;
    rp::latest_force.timestamp_us=rp::feedback.timestamp_us;
    ++rp::latest_force.sequence;
    rp::engine.update(rp::feedback.timestamp_us,rp::feedback,rp::latest_force);
    rp::publish();
}
static void start_engine_for_handoff(uint32_t session) {
    rehab::Config c;
    c.radius_m=0.02f; c.motor_turns_per_drum_turn=2; c.efficiency=0.8f;
    c.force_scale_n_per_count=0.001f; c.max_length_m=2; c.max_speed_m_s=1;
    c.max_force_n=200; c.max_torque_nm=2; c.torque_slew_nm_s=2;
    c.concentric_force_n=20; c.eccentric_force_n=10; c.speed_limit_m_s=0.4f;
    CHECK(rp::engine.configure(c)==Result::Ok);
    rp::latest_force.valid=true; rp::latest_force.raw=10000;
    advance_engine();
    CHECK(rp::engine.enable(session)==Result::Ok);
    // Exercise the actual engine without bypassing or changing the production
    // hardware lock: the mock motor remains idle/disarmed throughout.
    CHECK(rp::engine.start(session,rehab::Mode::Isotonic)==Result::Ok);
    for(unsigned i=0;i<10;++i)advance_engine();
    CHECK(rp::target.enable && rp::target.torque_nm<0 && !motors[0].is_armed_);
    rp::control_hook();
    CHECK(controllers[0].input_torque_<0);
}

static void test_hardware_lock() {
    reset_runtime();
    CHECK(!rehab::commissioning::may_energize);
    CHECK(!rp::motor_may_arm() && !rp::brake_may_arm());
    odrv.config_.enable_brake_resistor=true;
    axes[0].config_.startup_closed_loop_control=true;
    motors[0].config_.current_lim=10000;
    controllers[0].input_torque_=10000;
    CHECK(!rp::motor_may_arm() && !rp::axis_request_allowed(Axis::AXIS_STATE_MOTOR_CALIBRATION));
    Property<float> current_limit{&motors[0].config_.current_lim};
    current_limit.exchange(50000.0f);
    Property<bool> brake_enabled{&odrv.config_.enable_brake_resistor};
    brake_enabled.exchange(true);
    CHECK(motors[0].config_.current_lim==50000 && !rp::motor_may_arm() && !rp::brake_may_arm());
    active_target(); CHECK(!rp::motor_may_arm());
    motors[0].is_armed_=true; rp::fast_guard(); CHECK(!motors[0].is_armed_);
    rp::early_init();
    CHECK(!odrv.config_.enable_brake_resistor && !axes[0].config_.startup_closed_loop_control);
    CHECK(rp::diagnostic_word(10)==1);
    CHECK(rp::axis_request_allowed(Axis::AXIS_STATE_IDLE));
}
static void test_native_arbitration() {
    reset_runtime();
    {
        rehab::NativeWriteGuard writer;
        CHECK(writer && rp::native_writers==1);
        CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Busy);
        CHECK(!rp::owner_session);
    }
    CHECK(rp::native_writers==0);
    CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Ok);
    {
        rehab::NativeWriteGuard denied; CHECK(!denied && rp::native_rejected==1 && rp::native_writers==0);
        rehab::NativeWriteGuard read_only(false); CHECK(read_only);
    }
    CHECK(!rehab::native_input_allowed());
    Property<float> native_torque{&controllers[0].input_torque_};
    native_torque.exchange(100.0f); CHECK(native_torque.read()==0 && rp::native_rejected==2);
    Property<Axis::AxisState> state{&axes[0].requested_state_};
    state.exchange(Axis::AXIS_STATE_IDLE);
    CHECK((rp::latched&rehab::FaultStop) && !motors[0].is_armed_);
}
static void test_diagnostic_property_argument() {
    reset_runtime(); CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Ok);
    uint32_t argument=3;
    Property<uint32_t> staging{&argument};
    CHECK(staging.exchange(7u)==3 && argument==3 && rp::native_rejected==1);
    const auto diagnostic=staging.diagnostic_argument();
    CHECK(diagnostic.exchange(7u)==3 && argument==7 && rp::native_rejected==1);
    CHECK(staging.exchange(9u)==7 && argument==7 && rp::native_rejected==2);
    CHECK(staging.exchange(std::nullopt)==7 && rp::native_rejected==2);
    Property<float> native_input{&controllers[0].input_torque_};
    CHECK(native_input.exchange(1.0f)==0 && controllers[0].input_torque_==0 && rp::native_rejected==3);
    CHECK(rp::native_writers==0 && rp::owner_session==11 && !rp::latched);
}
static void test_handoff_preconditions() {
    reset_runtime(); axes[0].requested_state_=Axis::AXIS_STATE_MOTOR_CALIBRATION;
    CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Busy);
    reset_runtime(); axes[0].task_chain_[1]=Axis::AXIS_STATE_MOTOR_CALIBRATION;
    CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Busy);
    reset_runtime(); motors[0].is_armed_=true;
    CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Busy);
    reset_runtime(); axes[0].config_.enable_step_dir=true;
    CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Busy);
    reset_runtime(); axes[0].config_.enable_sensorless_mode=true;
    CHECK(rp::take_control(nullptr,11,rp::feedback)!=Result::Ok);
    reset_runtime(); controllers[0].anticogging_valid_=true; controllers[0].config_.anticogging.anticogging_enabled=true;
    CHECK(rp::take_control(nullptr,11,rp::feedback)!=Result::Ok);
    reset_runtime(); encoders[0].config_.mode=Encoder::MODE_HALL;
    CHECK(rp::take_control(nullptr,11,rp::feedback)!=Result::Ok);
    reset_runtime(); motors[0].config_.motor_type=Motor::MOTOR_TYPE_GIMBAL;
    CHECK(rp::take_control(nullptr,11,rp::feedback)!=Result::Ok);
    reset_runtime(); controllers[0].config_.load_encoder_axis=1;
    CHECK(rp::take_control(nullptr,11,rp::feedback)!=Result::Ok);
}
static void test_deadlines() {
    reset_runtime(); active_target(); rp::target.epoch=3; motors[0].is_armed_=true;
    rp::fast_guard(); CHECK((rp::latched&rehab::FaultTargetExpired) && !motors[0].is_armed_);
    reset_runtime(); active_target(); mock_now_ms=22; rp::fast_guard(); CHECK(rp::latched&rehab::FaultTargetExpired);
    reset_runtime(); active_target(); mock_now_ms=21; rp::fast_guard(); CHECK(!(rp::latched&rehab::FaultTargetExpired));
    reset_runtime(); active_target(); rp::limits.valid=false; rp::fast_guard(); CHECK(rp::latched&rehab::FaultTargetExpired);
}
static void fault_owned_session() {
    CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Ok);
    rp::engine.update(mock_now_ms*1000,rp::feedback,rp::latest_force);
    rp::engine.trip(rehab::FaultForce);
    rp::latched=rehab::FaultForce;
}
static void test_clear_race() {
    reset_runtime(); fault_owned_session();
    clear_injection=[] { rehab::native_emergency_stop(); };
    CHECK(rp::clear_control_faults(nullptr,11)!=Result::Ok);
    CHECK(rp::latched&rehab::FaultStop);
    CHECK(!rp::motor_may_arm());
    // A repeated fault has the same bit mask; only a generation check can
    // distinguish it from the old fault being cleared.
    reset_runtime(); fault_owned_session(); rp::engine.trip(rehab::FaultStop); rp::latched=rehab::FaultStop;
    clear_injection=[] { rehab::native_emergency_stop(); };
    CHECK(rp::clear_control_faults(nullptr,11)!=Result::Ok && (rp::latched&rehab::FaultStop));
}
static void test_clear_epoch() {
    reset_runtime(); fault_owned_session(); const uint32_t old=rp::owner_epoch;
    CHECK(rp::clear_control_faults(nullptr,11)==Result::Ok);
    CHECK(!rp::latched && !rp::target.enable && rp::owner_epoch==rp::engine.status().epoch && rp::owner_epoch!=old);
    CHECK(rp::engine.status().state==rehab::State::Standby && !rp::motor_may_arm());
}
static void test_fault_release_clears_native_inputs() {
    reset_runtime(); CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Ok);
    start_engine_for_handoff(11);
    rp::engine.trip(rehab::FaultForce); rp::latched=rehab::FaultForce; rp::publish();
    controllers[0].input_vel_=2.0f;
    rp::post_controller_hook();
    CHECK(controllers[0].input_torque_==0 && controllers[0].input_vel_==0);
    CHECK(controllers[0].torque_output_.present().value()==0);
    CHECK(rp::clear_control_faults(nullptr,11)==Result::Ok);
    CHECK(!rp::latched && rp::engine.status().state==rehab::State::Standby);
    // No ISR is required between a disabled target and RELEASE. Deliberately
    // retain old input values to verify release itself clears them atomically.
    controllers[0].input_torque_=-0.5f; controllers[0].input_vel_=2.0f;
    controllers[0].torque_setpoint_=-0.5f;
    CHECK(rp::release_control(nullptr,11,rp::feedback)==Result::Ok);
    CHECK(!rp::owner_session && !rp::engine.status().session && !rp::target.enable);
    CHECK(controllers[0].input_torque_==0 && controllers[0].input_vel_==0 && controllers[0].torque_setpoint_==0);
    CHECK(rehab::native_input_allowed() && !rp::motor_may_arm());
}
static void test_native_clear_worker() {
    reset_runtime(); rp::engine.trip(rehab::FaultNative); rp::latched=rehab::FaultNative; odrv.fault=true;
    odrv.clear_errors(); worker_once();
    CHECK(!rp::latched && !rp::engine.status().faults);
    CHECK(!motors[0].is_armed_ && !rp::target.enable && !rp::owner_session);
    CHECK(stack_scans==0); // stack scan is a low-rate diagnostic, not 1 kHz work
    rehab::native_emergency_stop(); worker_once();
    CHECK(rp::latched&rehab::FaultStop); // unowned engine must not erase hardware latch
}
static void test_post_controller_clamp() {
    reset_runtime(); controllers[0].torque_output_=3.0f; rp::post_controller_hook();
    CHECK(controllers[0].torque_output_.present().value()==3.0f); // Native untouched
    active_target(); controllers[0].torque_output_=-100.0f; rp::post_controller_hook();
    CHECK(controllers[0].torque_output_.present().value()==-0.5f);
    controllers[0].torque_output_=-0.25f; rp::post_controller_hook();
    CHECK(controllers[0].torque_output_.present().value()==-0.25f); // reduction preserved
    controllers[0].torque_output_=0.75f; rp::post_controller_hook();
    CHECK(controllers[0].torque_output_.present().value()==0.0f); // direction reversal blocked
    rp::target.torque_nm=0.7f; rp::limits.max_torque=0.3f;
    controllers[0].torque_output_=10.0f; rp::post_controller_hook();
    CHECK(std::fabs(controllers[0].torque_output_.present().value()-0.3f)<1e-6f);
    rp::target.enable=false; controllers[0].torque_output_=1.0f;
    controllers[0].input_torque_=1.0f; controllers[0].input_vel_=2.0f; rp::post_controller_hook();
    CHECK(controllers[0].torque_output_.present().value()==0.0f);
    CHECK(controllers[0].input_torque_==0 && controllers[0].input_vel_==0);
    rp::target.enable=true; controllers[0].torque_output_=NAN; rp::post_controller_hook();
    CHECK((rp::latched&rehab::FaultNative) && controllers[0].torque_output_.present().value()==0.0f);
    reset_runtime(); active_target(); controllers[0].torque_output_.reset(); rp::post_controller_hook();
    CHECK((rp::latched&rehab::FaultNative) && controllers[0].torque_output_.present().value()==0.0f);
}
static void test_reply_events() {
    reset_runtime(); submit(0,rw::Command::Hello);
    CHECK(saw_event(usb_event_queue,8));
    auto frames=replies(0); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));
    CHECK(frames[0].session==mock_rng.DR);
    events.clear(); submit(1,rw::Command::Hello);
    CHECK(saw_event(uart_event_queue,4));
    frames=replies(1); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Inhibited));
    ++mock_rng.DR; submit(1,rw::Command::Hello);
    frames=replies(1); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));
}
static void test_observer_stop_native_motion() {
    reset_runtime(); submit(1,rw::Command::Hello);
    auto frames=replies(1); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));
    const uint32_t observer=frames[0].session;
    CHECK(!rp::owner_session);
    motors[0].is_armed_=true; axes[0].current_state_=Axis::AXIS_STATE_CLOSED_LOOP_CONTROL;
    submit(1,rw::Command::Stop,observer,1);
    frames=replies(1); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));
    CHECK(!motors[0].is_armed_ && motors[0].disarm_calls>0);
    CHECK(axes[0].requested_state_==Axis::AXIS_STATE_IDLE && (rp::latched&rehab::FaultStop));
    CHECK(rp::engine.status().faults&rehab::FaultStop);
    CHECK(!rp::owner_session && !rp::motor_may_arm());
}
static void test_finish_and_release_native_inputs() {
    reset_runtime(); submit(0,rw::Command::Hello);
    auto frames=replies(0); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));
    const uint32_t owner=frames[0].session;
    submit(0,rw::Command::Acquire,owner,1);
    frames=replies(0); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));
    start_engine_for_handoff(owner);
    mock_rng.DR=0x23456789; submit(1,rw::Command::Hello);
    frames=replies(1); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));
    const uint32_t observer=frames[0].session;
    submit(1,rw::Command::Finish,observer,1);
    frames=replies(1); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::NotOwner));
    CHECK(rp::engine.status().state==rehab::State::Running && !rp::latched);
    submit(0,rw::Command::Finish,owner,2);
    frames=replies(0); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));
    CHECK(rp::engine.status().state==rehab::State::Stopping && !rp::latched && !rp::engine.status().faults);
    CHECK(rp::engine.status().target.enable && rp::engine.status().target.torque_nm<0);
    float previous=std::fabs(rp::engine.status().target.torque_nm);
    for(unsigned i=0;i<30 && rp::engine.status().state==rehab::State::Stopping;++i) {
        advance_engine();
        const float magnitude=std::fabs(rp::engine.status().target.torque_nm);
        CHECK(magnitude<=previous && previous-magnitude<=rp::engine.config().torque_slew_nm_s*0.001f+1e-6f);
        previous=magnitude;
    }
    CHECK(rp::engine.status().state==rehab::State::Standby && !rp::target.enable && !rp::latched);
    controllers[0].input_vel_=2.0f; rp::post_controller_hook();
    CHECK(controllers[0].input_torque_==0 && controllers[0].input_vel_==0);
    controllers[0].input_torque_=-0.5f; controllers[0].input_vel_=2.0f; controllers[0].torque_setpoint_=-0.5f;
    submit(0,rw::Command::Release,owner,3);
    frames=replies(0); CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));
    CHECK(!rp::owner_session && !rp::engine.status().session && !rp::target.enable);
    CHECK(controllers[0].input_torque_==0 && controllers[0].input_vel_==0 && controllers[0].torque_setpoint_==0);
    rp::disconnected(0); rp::service_transports();
    CHECK(!rp::latched && !rp::engine.status().faults);
}
static void test_disconnect_flush() {
    reset_runtime(); submit(0,rw::Command::Hello); CHECK(rp::tx[0].size()>0);
    rp::disconnected(0); rp::service_transports();
    CHECK(replies(0).empty());
    submit(0,rw::Command::Hello); const auto frames=replies(0);
    CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));
}
static void test_empty_poll_expiry() {
    reset_runtime(); const uint8_t partial[]={'R','H',rw::kVersion,0};
    rp::receive_bytes(0,partial,sizeof(partial)); rp::service_transports();
    for(unsigned i=0;i<300;++i) { ++mock_now_ms; rp::service_transports(); }
    CHECK(rp::parsers[0].expired==1);
    submit(0,rw::Command::Hello); CHECK(replies(0).size()==1);
}
static void test_receive_overflow() {
    reset_runtime(); active_target(); uint8_t bytes[513]{};
    rp::receive_bytes(0,bytes,sizeof(bytes));
    CHECK(rp::rx_overflow==1 && (rp::latched&rehab::FaultPlatform));
}
static uint32_t negotiate(unsigned port) {
    mock_rng.DR=0xABCD0000u+port;
    submit(port,rw::Command::Hello);const auto frames=replies(port);
    CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Ok));return frames[0].session;
}
static rw::PendingCommand queued(unsigned port,uint32_t session,uint32_t sequence,rw::Command command) {
    rw::PendingCommand p;p.port=port;p.frame.session=session;p.frame.sequence=sequence;
    p.frame.command=uint16_t(command);p.received_us=mock_now_ms*1000;p.epoch=rp::owner_epoch;
    p.fault_generation=rp::fault_generation;p.transport_generation=rp::transport_generation[port];return p;
}
static void test_command_fifo() {
    rw::CommandQueue q;rw::PendingCommand p;
    CHECK(!q.pop(p));
    for(unsigned round=0;round<1000;++round) {
        for(unsigned i=0;i<q.capacity;++i) {
            p.frame.sequence=round*q.capacity+i;p.frame.payload[255]=uint8_t(i);
            p.port=i%3;p.epoch=round;p.received_us=round*13;p.fault_generation=round+9;
            CHECK(q.push(p));
        }
        CHECK(!q.push(p) && q.size()==q.capacity && q.highwater()==q.capacity);
        for(unsigned i=0;i<q.capacity;++i) {
            CHECK(q.pop(p));CHECK(p.frame.sequence==round*q.capacity+i && p.frame.payload[255]==i);
            CHECK(p.port==i%3 && p.epoch==round && p.received_us==round*13 && p.fault_generation==round+9);
        }
        CHECK(!q.pop(p));
    }
}
static void test_command_capacity_and_retry() {
    reset_runtime();uint32_t sessions[3];for(unsigned port=0;port<3;++port)sessions[port]=negotiate(port);
    for(unsigned port=0;port<3;++port)for(unsigned seq=1;seq<=3;++seq)send_bytes(port,rw::Command::Status,sessions[port],seq);
    rp::service_transports();
    CHECK(rp::commands.size()==4 && rp::commands.highwater()==8 && rp::command_queue_full==1);
    auto p0=replies(0),p1=replies(1),p2=replies(2);
    CHECK(p0.size()==3 && p1.size()==1 && p2.size()==1);
    CHECK(p2[0].sequence==3 && result_code(p2[0])==uint32_t(Result::QueueFull));
    rp::service_transports();p1=replies(1);p2=replies(2);
    CHECK(p1.size()==2 && p2.size()==2 && rp::commands.size()==0);
    CHECK(result_code(p2[0])==uint32_t(Result::Ok) && result_code(p2[1])==uint32_t(Result::Ok));
    // An unaccepted request can be retried without skipping older queued work.
    submit(2,rw::Command::Status,sessions[2],3);p2=replies(2);
    CHECK(p2.size()==1 && result_code(p2[0])==uint32_t(Result::Ok));
    CHECK(rp::diagnostic_word(48)==0 && rp::diagnostic_word(49)==8 && rp::diagnostic_word(50)==1 && rp::diagnostic_word(52)==8);
}
static void test_urgent_stop_queue_and_replay() {
    reset_runtime();const auto owner=negotiate(0),observer=negotiate(1);
    submit(0,rw::Command::Acquire,owner,1);CHECK(result_code(replies(0)[0])==uint32_t(Result::Ok));
    for(unsigned i=0;i<8;++i)CHECK(rp::commands.push(queued(0,owner,i+2,rw::Command::ClearFaults)));
    motors[0].is_armed_=true;
    // Invalid session and payload never acquire the emergency-stop privilege.
    submit(1,rw::Command::Stop,observer+1,1);
    CHECK(result_code(replies(1)[0])==uint32_t(Result::BadSequence) && !rp::latched);
    for(unsigned i=0;i<4;++i)CHECK(rp::commands.push(queued(0,owner,i+10,rw::Command::ClearFaults)));
    submit(1,rw::Command::Stop,observer,1);
    CHECK(result_code(replies(1)[0])==uint32_t(Result::Ok) && (rp::latched&rehab::FaultStop));
    CHECK(!motors[0].is_armed_ && rp::commands.size()==4 && rp::command_stale==4);
    const auto generation=rp::fault_generation,disarms=motors[0].disarm_calls;
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    submit(1,rw::Command::Stop,observer,1);
    CHECK(result_code(replies(1)[0])==uint32_t(Result::Ok));
    CHECK(rp::fault_generation==generation && motors[0].disarm_calls==disarms && rp::latched);
    CHECK(rp::commands.size()==0 && rp::command_stale==8);
    // The old queued CLEARs cannot clear the new STOP. A fresh explicit CLEAR can.
    replies(0);submit(0,rw::Command::ClearFaults,owner,20);
    CHECK(result_code(replies(0)[0])==uint32_t(Result::Ok) && !rp::latched && !rp::engine.status().target.enable);
}
static void test_command_epoch_age_and_disconnect() {
    reset_runtime();const auto session=negotiate(0);
    send_bytes(0,rw::Command::Acquire,session,1);send_bytes(0,rw::Command::Enable,session,2);
    rp::service_transports();auto frames=replies(0);
    CHECK(frames.size()==2 && result_code(frames[0])==uint32_t(Result::Ok) && result_code(frames[1])==uint32_t(Result::Stale));
    CHECK(!rp::engine.status().target.enable && rp::command_stale==1);
    CHECK(rp::commands.push(queued(0,session,3,rw::Command::Status)));mock_now_ms+=21;
    rp::service_transports();frames=replies(0);CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Stale));
    CHECK(rp::commands.push(queued(0,session,4,rw::Command::ClearFaults)));
    rp::disconnected(0);rp::service_transports();
    CHECK(replies(0).empty() && rp::commands.size()==0 && rp::engine.status().state==rehab::State::Fault);
    CHECK(rp::engine.status().session==session && !rp::engine.status().target.enable);
    // Unsigned age subtraction remains bounded across the microsecond wrap.
    reset_runtime();const auto fresh=negotiate(0);auto p=queued(0,fresh,1,rw::Command::Status);
    p.received_us=UINT32_MAX-999;mock_now_ms=1;CHECK(rp::commands.push(p));rp::service_transports();
    CHECK(result_code(replies(0)[0])==uint32_t(Result::Ok));
}
static void test_clear_dispatch_race() {
    reset_runtime();CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Ok);
    rp::dispatch_fault_generation=rp::fault_generation;rp::dispatching_command=true;
    rp::trip(rehab::FaultStop);axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    CHECK(rp::clear_control_faults(nullptr,11)==Result::Stale && rp::latched);
    CHECK(!rp::motor_may_arm());rp::dispatching_command=false;
}
static void test_byte_ingress_fault_boundary() {
    reset_runtime();const auto session=negotiate(0);
    submit(0,rw::Command::Acquire,session,1);replies(0);
    for(unsigned seq=2;seq<9;++seq)send_bytes(0,rw::Command::ClearFaults,session,seq);
    rp::trip(rehab::FaultStop);rp::engine.trip(rehab::FaultStop);
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    rp::service_transports();CHECK(replies(0).empty() && rp::latched && rp::rx[0].size()==0);
    // Commands received after the event survive its frozen read cutoff.
    submit(0,rw::Command::ClearFaults,session,10);
    CHECK(result_code(replies(0)[0])==uint32_t(Result::Ok) && !rp::latched);
    // An uncommitted producer copy also cannot cross the ISR boundary.
    CriticalSectionContext::before_enter=[](){rp::trip(rehab::FaultStop);rp::engine.trip(rehab::FaultStop);};
    send_bytes(0,rw::Command::ClearFaults,session,11);
    CHECK(rp::rx[0].size()==0 && rp::latched && rp::rx_overflow==0);
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;rp::service_transports();CHECK(replies(0).empty());
    // STOP and CLEAR already in one parser chunk preserve the old generation.
    send_bytes(0,rw::Command::Stop,session,12);send_bytes(0,rw::Command::ClearFaults,session,13);
    rp::service_transports();const auto frames=replies(0);
    CHECK(frames.size()==2 && result_code(frames[0])==uint32_t(Result::Ok));
    CHECK(result_code(frames[1])==uint32_t(Result::Stale) && rp::latched);
}
static void test_fault_mirror_does_not_starve_queue() {
    reset_runtime();const auto session=negotiate(0);submit(0,rw::Command::Acquire,session,1);replies(0);
    rp::trip(rehab::FaultStop);rp::engine.trip(rehab::FaultStop);
    const auto generation=rp::fault_generation;
    for(unsigned i=0;i<7;++i)CHECK(rp::commands.push(queued(0,session,i+2,rw::Command::Status)));
    CHECK(rp::commands.push(queued(0,session,9,rw::Command::ClearFaults)));
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;worker_once();replies(0);
    CHECK(rp::commands.size()==4 && rp::fault_generation==generation);
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;worker_once();auto frames=replies(0);
    CHECK(frames.size()==4 && result_code(frames.back())==uint32_t(Result::Ok));
    CHECK(!rp::latched && !rp::command_stale && !rp::engine.status().target.enable);
}
static void test_new_engine_fault_before_dispatch() {
    reset_runtime();const auto session=negotiate(0);submit(0,rw::Command::Acquire,session,1);replies(0);
    start_engine_for_handoff(session);
    CHECK(rp::commands.push(queued(0,session,2,rw::Command::ClearFaults)));
    mock_now_ms+=251;worker_once();const auto frames=replies(0);
    CHECK(frames.size()==1 && result_code(frames[0])==uint32_t(Result::Stale));
    CHECK((rp::latched&rehab::FaultHeartbeat) && rp::engine.status().state==rehab::State::Fault);
    CHECK(!motors[0].is_armed_ && !rp::engine.status().target.enable);
}
static void test_partial_frame_fault_boundaries() {
    reset_runtime();const auto session=negotiate(0);submit(0,rw::Command::Acquire,session,1);replies(0);
    rw::Frame clear;clear.command=uint16_t(rw::Command::ClearFaults);clear.session=session;clear.sequence=2;
    uint8_t bytes[rw::kMaxFrame];const size_t size=rw::encode(clear,bytes,sizeof(bytes));CHECK(size==20);
    rp::receive_bytes(0,bytes,10);rp::service_transports();CHECK(replies(0).empty());
    rp::trip(rehab::FaultStop);rp::engine.trip(rehab::FaultStop);
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    rp::receive_bytes(0,bytes+10,size-10);rp::service_transports();
    CHECK(replies(0).empty() && rp::latched);
    submit(0,rw::Command::ClearFaults,session,3);CHECK(result_code(replies(0)[0])==uint32_t(Result::Ok));
    clear.sequence=5;rw::encode(clear,bytes,sizeof(bytes));
    send_bytes(0,rw::Command::Stop,session,4);rp::receive_bytes(0,bytes,10);rp::service_transports();
    CHECK(result_code(replies(0)[0])==uint32_t(Result::Ok));
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    rp::receive_bytes(0,bytes+10,size-10);rp::service_transports();
    CHECK(replies(0).empty() && rp::latched);
    submit(0,rw::Command::ClearFaults,session,6);CHECK(result_code(replies(0)[0])==uint32_t(Result::Ok) && !rp::latched);
}
#if defined(REHAB_V13)
static void test_physical_stop_revision() {
    reset_runtime();rp::early_init();
    CHECK(stop_port==GPIOC && stop_pin==GPIO_PIN_4 && stop_callback);
    CHECK(rp::diagnostic_word(63)==0x1301 && rp::diagnostic_word(64)==1 && rp::diagnostic_word(65)==3);
    CHECK(rp::diagnostic_word(75)==0 && rp::diagnostic_word(76)==1);
    active_target();motors[0].is_armed_=true;
    const auto fault_before=rp::fault_generation;
    mock_gpio_c.IDR=0;stop_callback(nullptr);
    CHECK(!motors[0].is_armed_ && !gate_ready && rp::latched&rehab::FaultStop);
    CHECK(rp::fault_generation>fault_before && rp::diagnostic_word(74)==2);
    const auto epoch=gate_generation;
    rp::fast_guard();rp::fast_guard();CHECK(gate_generation==epoch);
    mock_gpio_c.IDR=GPIO_PIN_4;stop_callback(nullptr);
    CHECK(gate_generation>epoch && rp::latched && !rp::motor_may_arm());
    // A coalesced low/high pulse still invalidates the DRV cache at ISR entry.
    gate_ready=true;stop_callback(nullptr);CHECK(!gate_ready && rp::latched);
    // Startup with an open NC circuit faults immediately; high later never clears it.
    reset_runtime();mock_gpio_c.IDR=0;rp::early_init();
    CHECK(rp::latched&rehab::FaultStop && !gate_ready);
    mock_gpio_c.IDR=GPIO_PIN_4;CHECK(rp::latched && !rp::motor_may_arm());
    reset_runtime();worker_once();CHECK(board_spi_services==1);
}
#endif
int main() {
    struct Test { const char* name; void (*run)(); } tests[] = {
#if defined(REHAB_V13)
        {"V1.3 physical STOP startup/edges/generation",test_physical_stop_revision},
#endif
        {"hardware lock",test_hardware_lock},{"native arbitration",test_native_arbitration},
        {"diagnostic argument property copy",test_diagnostic_property_argument},
        {"handoff preconditions",test_handoff_preconditions},{"target deadlines",test_deadlines},
        {"fault clear race",test_clear_race},{"fault clear epoch",test_clear_epoch},
        {"fault release clears native inputs",test_fault_release_clears_native_inputs},
        {"native clear worker",test_native_clear_worker},{"response wake events",test_reply_events},
        {"final controller torque clamp",test_post_controller_clamp},
        {"observer STOP disarms native motor",test_observer_stop_native_motion},
        {"owner FINISH and release",test_finish_and_release_native_inputs},
        {"disconnect TX flush",test_disconnect_flush},{"empty poll expiry",test_empty_poll_expiry},
        {"RX overflow",test_receive_overflow},
        {"complete-command FIFO wrap and capacity",test_command_fifo},
        {"command budget, overflow and retry",test_command_capacity_and_retry},
        {"urgent STOP, queued CLEAR and replay",test_urgent_stop_queue_and_replay},
        {"command epoch, age and disconnect",test_command_epoch_age_and_disconnect},
        {"CLEAR interrupted after dispatch validation",test_clear_dispatch_race},
        {"raw and uncommitted RX fault boundary",test_byte_ingress_fault_boundary},
        {"existing fault does not starve command queue",test_fault_mirror_does_not_starve_queue},
        {"new application fault invalidates queued CLEAR",test_new_engine_fault_before_dispatch},
        {"partial frame across STOP boundaries",test_partial_frame_fault_boundaries},
    };
    for (const auto& test : tests) {
        try { test.run(); std::printf("PASS %s\n",test.name); }
        catch (const TestFailure&) { std::printf("FAIL %s\n",test.name); }
    }
    std::printf("runtime integration: %u checks, %u failed; actual runtime/core/protocol, mocked HAL/RTOS/ODrive carriers\n",checks,failures);
    return failures ? 1 : 0;
}
