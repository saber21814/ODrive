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
#include "Rehab/platform/runtime_v13.cpp"

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
#if REHAB_ENABLE_APPLICATION
using Result = rehab::Result;
#else
using Result = rw::PeripheralResult;
#endif
static void reset_runtime() {
    mock_now_ms=1; mock_primask=mock_ipsr=0; clear_injection=nullptr; events.clear();
    motors[0]=Motor{};controllers[0]=Controller{};encoders[0]=Encoder{};odrv=ODrive{};
    axes[0].config_=Axis::Config{};axes[0].error_=0;
    axes[0].task_chain_.fill(Axis::AXIS_STATE_UNDEFINED);
    axes[0].current_state_=Axis::AXIS_STATE_IDLE;axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    axes[0].watchdog_feeds=0;mock_gpio_c.IDR=GPIO_PIN_4;gate_ready=true;
    rp::latched=rp::fault_generation=rp::disconnect_bits=0;
    rp::commands={};rp::command_queue_full=rp::command_stale=0;
    rp::boundary_revision=0;rp::dispatching_command=false;rp::started=false;
    for(unsigned i=0;i<3;++i){rp::rx[i]={};rp::tx[i]={};rp::parsers[i]={};rp::rx_boundary[i]=rp::boundary_seen[i]=rp::transport_generation[i]=0;}
#if REHAB_ENABLE_APPLICATION
    rp::engine=rehab::Engine{};rp::feedback={};rp::published={};rp::target={};rp::limits={};
    rp::owner_session=rp::native_writers=rp::native_rejected=0;rp::owner_epoch=1;rp::owner_port=3;
    rp::training_modes_set=rp::native_clear_requested=false;rp::owner_heartbeat_us=0;
    rp::service.~Service();new (&rp::service) rw::Service(rp::engine,rp::hooks);
#else
    rp::service=rw::PeripheralService(rp::hooks);
#endif
    rp::pending_network={};rp::network_revision=rp::network_seen=0;
    CriticalSectionContext::before_enter=nullptr;
}
static void idle_native() {
    axes[0].task_chain_.fill(Axis::AXIS_STATE_UNDEFINED);
    axes[0].current_state_=Axis::AXIS_STATE_IDLE;axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    motors[0].is_armed_=false;
}
static void worker_once() { try {rp::safety_main(nullptr);}catch(const EndIteration&){} }
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
#ifndef TRAINING_FIXTURE_ONLY
static void test_queue_boundaries() {
    reset_runtime();rp::early_init();idle_native();
    submit(0,rw::Command::Hello);auto hello=replies(0);CHECK(hello.size()==1);
    const auto session=hello[0].session;CHECK(session!=0);
    submit(0,rw::Command::Acquire,session,2);auto rejected=replies(0);
    CHECK(rejected.size()==1 && result_code(rejected[0])==uint32_t(Result::Unsupported));
    CHECK(rp::diagnostic_word(14)>0 && !motors[0].is_armed_);
    rw::PendingCommand queued{};queued.port=0;queued.frame.command=19;
    queued.frame.session=session;queued.frame.sequence=3;queued.frame.length=14;
    const uint8_t cfg[]={192,168,11,3,0,0,0,0,255,255,255,0,0xe8,0x99};
    std::memcpy(queued.frame.payload,cfg,14);queued.received_us=rp::now_us();
    queued.fault_generation=rp::fault_generation;queued.transport_generation=rp::transport_generation[0];
    CHECK(rp::commands.push(queued));const auto rev=rp::network_revision;
    rp::trip(rp::FaultStop);rp::service_transports();auto stale=replies(0);
    CHECK(stale.size()==1 && result_code(stale[0])==uint32_t(Result::Stale));
    CHECK(rp::network_revision==rev && rp::pending_network.ip[3]==2);
    // A transport disconnect discards its old queue, without stopping native control.
    idle_native();rp::native_clear_faults();queued.frame.sequence=4;
    queued.fault_generation=rp::fault_generation;CHECK(rp::commands.push(queued));
    motors[0].is_armed_=true;rp::disconnected(0);rp::service_transports();
    CHECK(replies(0).empty() && motors[0].is_armed_ && rp::network_revision==rev);
    idle_native();
    // Explicit valid network STOP is still immediate and latching.
    mock_rng.DR+=1;submit(0,rw::Command::Hello);hello=replies(0);CHECK(hello.size()==1);
    motors[0].is_armed_=true;submit(0,rw::Command::Stop,hello[0].session,2);
    auto stopped=replies(0);CHECK(stopped.size()==1 && result_code(stopped[0])==0);
    CHECK(!motors[0].is_armed_ && rp::latched);
}
int main() {
 try {
    test_preserve_parameters_and_boot();
    test_queue_boundaries();
    reset_runtime();rp::early_init();idle_native();
    CHECK(stop_port==GPIOC && stop_pin==GPIO_PIN_4);
    CHECK(rp::motor_may_arm() && rp::brake_may_arm());
    Property<float> prop{&motors[0].config_.current_lim};prop.exchange(1.5f);
    CHECK(motors[0].config_.current_lim==1.5f && rehab::native_input_allowed());
    for(unsigned state:{0u,1u,4u,7u,8u,9u}) CHECK(rp::axis_request_allowed(state));
    controllers[0].input_torque_=0.123f;controllers[0].torque_output_=0.321f;
    rp::control_hook();rp::post_controller_hook();
    CHECK(controllers[0].input_torque_==0.123f && controllers[0].torque_output_.present().value()==0.321f);
    motors[0].is_armed_=true;mock_now_ms+=20000;rp::fast_guard();
    rp::disconnected(2);rp::service_transports();
    CHECK(motors[0].is_armed_ && axes[0].watchdog_feeds==0);
    mock_gpio_c.IDR=0;stop_callback(nullptr);
    CHECK(!motors[0].is_armed_ && rp::latched && !gate_ready);
    CHECK(!rp::axis_request_allowed(4) && rp::brake_may_arm());
    idle_native();rp::native_clear_faults();CHECK(rp::latched);
    mock_gpio_c.IDR=GPIO_PIN_4;gate_ready=true;
    CHECK(!rp::motor_may_arm());rp::native_clear_faults();
    CHECK(!rp::latched && rp::motor_may_arm() && !motors[0].is_armed_);
    CHECK(axes[0].requested_state_==Axis::AXIS_STATE_UNDEFINED);
    motors[0].is_armed_=true;gate_ready=false;rp::fast_guard();
    CHECK(!motors[0].is_armed_ && rp::latched);
    gate_ready=true;CHECK(!rp::motor_may_arm());
    reset_runtime();mock_gpio_c.IDR=0;rp::early_init();
    CHECK(rp::latched && !rp::motor_may_arm());
    reset_runtime();rp::early_init();idle_native();
    unsigned before=rp::latest_force.sequence;worker_once();
    CHECK(rp::latest_force.sequence>before && board_spi_services>0);
    const uint8_t cfg[]={192,168,11,2,0,0,0,0,255,255,255,0,0xe8,0x99};
    CHECK(rp::set_network(nullptr,cfg,14)==Result::Ok);
    motors[0].is_armed_=true;CHECK(rp::set_network(nullptr,cfg,14)!=Result::Ok);idle_native();
    rp::dispatching_command=true;rp::dispatch_fault_generation=rp::fault_generation;
    ++rp::fault_generation;CHECK(rp::set_network(nullptr,cfg,14)==Result::Stale);
    for(unsigned i:{0u,1u,3u,4u,5u,6u,8u,9u,17u,18u,28u,53u,62u}) CHECK(rp::diagnostic_word(i)==0);
    CHECK(std::isnan(rp::diagnostic_float(0)) && rp::diagnostic_float(8)==24);
    std::printf("PASS: %u native peripheral runtime checks\n",checks);
 } catch(const TestFailure&) {return 1;}
 return 0;
}

#endif // TRAINING_FIXTURE_ONLY
