// Actual firmware ingress functions + actual generated Fibre stubs + runtime.
// Run ingress_sources.py first; see ingress/README.md for the exact boundary.
#include "ingress/mocks/odrive_main.h"
#include "ingress/mocks/freertos_vars.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <tuple>
#include <variant>
#include <vector>
#include <fibre/introspection.hpp>
#include "fibre-cpp/stream_utils.hpp"
#include "communication/can/canbus.hpp"
// Only visibility is changed, not the production class declarations/bodies.
#define private public
#include "communication/can/can_simple.hpp"
#include "communication/ascii_protocol.hpp"
#undef private
#include "autogen/function_stubs.hpp"
#include "Rehab/platform/runtime.cpp"

GPIO_TypeDef mock_gpio_a, mock_gpio_b, mock_gpio_c;
SPI_TypeDef mock_spi3;
DMA_Stream_TypeDef mock_streams[8];
MockDwt mock_dwt;
MockCoreDebug mock_debug;
MockNvic mock_nvic;
MockRng mock_rng;
uint32_t mock_primask=0, mock_ipsr=0, mock_flags_cleared=0, mock_cycle_step=1;
Motor motors[1]; Controller controllers[1]; Encoder encoders[1]; Axis axes[1];
ODrive odrv; MockThermistor fet_thermistors[1]; MockSpiArbiter ext_spi_arbiter;
float vbus_voltage=24, ibus_=0;
const char serial_number_str[]="INGRESS-HOST";
osMessageQId usb_event_queue=nullptr, uart_event_queue=nullptr;
static uint32_t mock_now_ms=1;
static float analog_voltage=1.65f;
static unsigned resets=0, subscriptions=0, unsubscriptions=0;
static bool subscribe_ok=true;
static unsigned checks=0, failures=0;
struct TestFailure {};
struct EndIteration {};
static void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr,"FAIL ingress_tests.cpp:%d: %s\n",line,expression); throw TestFailure{}; }
}
#define CHECK(condition) check(bool(condition), #condition, __LINE__)
uint32_t HAL_GetTick() { return mock_now_ms; }
bool Stm32Gpio::config(uint32_t,uint32_t,uint32_t) { return true; }
bool Stm32Gpio::subscribe(bool,bool,void(*)(void*),void*) { ++subscriptions; return subscribe_ok; }
void Stm32Gpio::unsubscribe() { ++unsubscriptions; }
osThreadId osThreadCreate(const MockThreadDefinition*,void*) { return reinterpret_cast<void*>(3); }
uint32_t xTaskGetTickCount() { return mock_now_ms; }
uint32_t uxTaskGetStackHighWaterMark(osThreadId) { return 64; }
void vTaskDelayUntil(uint32_t* tick,uint32_t delay) { *tick+=delay; mock_now_ms+=delay; throw EndIteration{}; }
int osMessagePut(osMessageQId,uint32_t,uint32_t) { return 0; }
void NVIC_SystemReset() { ++resets; }
float rehab_board_adc_voltage(uint32_t) { return 1.65f; }
Stm32Gpio get_gpio(size_t number) { return number<GPIO_COUNT?Stm32Gpio{GPIOA,GPIO_PIN_1}:Stm32Gpio{}; }
float get_adc_voltage(Stm32Gpio) { return analog_voltage; }
void ODrive::clear_errors() {
    ++clears;
    rehab::platform::native_clear_faults();
    fault=false;
}
namespace rp=rehab::platform;
using rehab::Result;

// Minimal property directory: real Property and real Fibre conversion/type-info
// implementations below it. No admission decision is made by this directory.
static Introspectable torque_property() {
    return FibrePropertyTypeInfo<Property<float>>::make_introspectable(Property<float>{&controllers[0].input_torque_});
}
static Introspectable current_property() {
    return FibrePropertyTypeInfo<Property<float>>::make_introspectable(Property<float>{&motors[0].config_.current_lim});
}
static Introspectable state_property() {
    return FibrePropertyTypeInfo<Property<Axis::AxisState>>::make_introspectable(Property<Axis::AxisState>{&axes[0].requested_state_});
}
struct TestPropertyDirectory {
    Introspectable get_child(const char* name,size_t) {
        if (!std::strcmp(name,"axis0.controller.input_torque")) return torque_property();
        if (!std::strcmp(name,"axis0.motor.config.current_lim")) return current_property();
        if (!std::strcmp(name,"axis0.requested_state")) return state_property();
        return {};
    }
} root_obj;
namespace fibre {
const uint16_t json_crc_=0x5a31;
static void get_property(Introspectable& output,size_t index) {
    if (index==1) output=torque_property();
    else if (index==2) output=current_property();
    else if (index==3) output=state_property();
}
}
using namespace fibre;
#include "can.inc"
#include "ascii.inc"
#include "axis.inc"
#include "mapping.inc"

class CapturingCan : public CanBusBase {
public:
    std::vector<can_Message_t> sent;
    bool send_message(const can_Message_t& message) override { sent.push_back(message); return true; }
    bool subscribe(const MsgIdFilterSpecs&,on_can_message_cb_t,void*,CanSubscription**) override { return true; }
    bool unsubscribe(CanSubscription*) override { return true; }
};
class CapturingSink : public AsyncStreamSink {
public:
    std::string text;
    void start_write(cbufptr_t bytes,TransferHandle* handle,Callback<void,WriteResult> done) override {
        if(handle)*handle=1;
        text.append(reinterpret_cast<const char*>(bytes.begin()),bytes.size());
        done.invoke({kStreamOk,bytes.end()});
    }
    void cancel_write(TransferHandle) override {}
};
static void ascii(AsciiProtocol& protocol,const char* command) {
    protocol.process_line({reinterpret_cast<const uint8_t*>(command),std::strlen(command)});
}
static void reset_fixture() {
    mock_now_ms=1; mock_primask=mock_ipsr=0; mock_gpio_a.IDR=mock_gpio_b.IDR=mock_gpio_c.IDR=0;
    motors[0]=Motor{}; controllers[0]=Controller{}; encoders[0]=Encoder{};
    odrv.~ODrive(); new (&odrv) ODrive{};
    axes[0].config_=Axis::Config{}; axes[0].task_chain_.fill(Axis::AXIS_STATE_UNDEFINED);
    axes[0].current_state_=Axis::AXIS_STATE_IDLE; axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED;
    axes[0].steps_=0; axes[0].step_dir_active_=false; axes[0].watchdog_feeds=axes[0].error_=0;
    axes[0].trap_traj_=Axis::Trajectory{}; axes[0].sensorless_estimator_=Axis::Sensorless{};
    subscriptions=unsubscriptions=resets=0; subscribe_ok=true; analog_voltage=1.65f;
    rp::engine.reset(); rp::feedback={}; rp::feedback.timestamp_us=1000; rp::feedback.vbus_v=24;
    rp::published={}; rp::target={}; rp::latest_force={}; rp::limits={};
    rp::owner_session=0; rp::owner_epoch=1; rp::native_writers=rp::native_rejected=rp::latched=0;
    rp::fault_generation=rp::native_clear_requested=rp::disconnect_bits=0;
    rp::rx_overflow=rp::tx_overflow=rp::command_overflow=0;
    rp::log_read=rp::log_write=rp::log_count=rp::log_dropped=0;
    rp::commands={}; rp::command_queue_full=rp::command_stale=0;
    rp::boundary_revision=0; rp::dispatching_command=false; rp::dispatch_fault_generation=0;
    for(unsigned port=0;port<3;++port) {
        rp::rx[port]={}; rp::tx[port]={}; rp::parsers[port]={};
        rp::transport_generation[port]=rp::rx_boundary[port]=rp::boundary_seen[port]=0;
    }
    CriticalSectionContext::before_enter=nullptr;
}
enum class Phase { Native, Running, Paused };
static void tick_engine() {
    ++mock_now_ms; rp::feedback.timestamp_us=mock_now_ms*1000;
    rp::latest_force.timestamp_us=rp::feedback.timestamp_us; ++rp::latest_force.sequence;
    rp::engine.update(rp::feedback.timestamp_us,rp::feedback,rp::latest_force); rp::publish();
}
static void prepare_phase(Phase phase) {
    reset_fixture();
    if(phase==Phase::Native)return;
    CHECK(rp::take_control(nullptr,11,rp::feedback)==Result::Ok);
    rehab::Config c;
    c.radius_m=.02f; c.motor_turns_per_drum_turn=2; c.efficiency=.8f;
    c.force_scale_n_per_count=.001f; c.max_length_m=2; c.max_speed_m_s=1;
    c.max_force_n=200; c.max_torque_nm=2; c.torque_slew_nm_s=2;
    c.concentric_force_n=20; c.eccentric_force_n=10; c.speed_limit_m_s=.4f;
    CHECK(rp::engine.configure(c)==Result::Ok);
    rp::latest_force.valid=true; rp::latest_force.raw=10000; tick_engine();
    CHECK(rp::engine.enable(11)==Result::Ok);
    CHECK(rp::engine.start(11,rehab::Mode::Isotonic)==Result::Ok);
    tick_engine();
    if(phase==Phase::Paused) {
        CHECK(rp::engine.pause(11)==Result::Ok);
        for(unsigned i=0;i<10 && rp::engine.status().state==rehab::State::Stopping;++i)tick_engine();
        CHECK(rp::engine.status().state==rehab::State::Paused);
    } else CHECK(rp::engine.status().state==rehab::State::Running);
    CHECK(rp::owner_session==11 && !motors[0].is_armed_);
}
static auto mutation_snapshot() {
    const auto& c=controllers[0]; const auto& a=axes[0];
    return std::make_tuple(c.input_pos_,c.input_vel_,c.input_torque_,c.config_.control_mode,c.config_.input_mode,
        c.config_.vel_limit,c.config_.inertia,c.config_.pos_gain,c.config_.vel_gain,c.config_.vel_integrator_gain,
        c.input_updates,c.calibrations,c.mode_updates,c.config_.anticogging.calib_anticogging,
        motors[0].config_.current_lim,motors[0].config_.torque_lim,encoders[0].shadow_count_,encoders[0].count_writes,
        a.config_.can.node_id,a.requested_state_,a.watchdog_feeds,a.steps_,a.trap_traj_.config_.vel_limit,
        a.trap_traj_.config_.accel_limit,a.trap_traj_.config_.decel_limit,
        odrv.clears,odrv.saves,odrv.erases,odrv.reboots,odrv.dfu,resets);
}
static can_Message_t request(uint32_t command) {
    can_Message_t message;
    message.id=(axes[0].config_.can.node_id<<5)|command;
    can_setSignal<float>(message,2.5f,0,32,true);
    can_setSignal<float>(message,1.25f,32,32,true);
    if(command==CANSimple::MSG_SET_AXIS_REQUESTED_STATE)can_setSignal<int32_t>(message,Axis::AXIS_STATE_MOTOR_CALIBRATION,0,32,true);
    if(command==CANSimple::MSG_SET_CONTROLLER_MODES) {
        can_setSignal<int32_t>(message,Controller::CONTROL_MODE_TORQUE_CONTROL,0,32,true);
        can_setSignal<int32_t>(message,Controller::INPUT_MODE_PASSTHROUGH,32,32,true);
    }
    if(command==CANSimple::MSG_SET_AXIS_NODE_ID || command==CANSimple::MSG_SET_LINEAR_COUNT)
        can_setSignal<int32_t>(message,42,0,32,true);
    return message;
}
static void test_can_all_commands() {
    const uint32_t reads[]={CANSimple::MSG_GET_MOTOR_ERROR,CANSimple::MSG_GET_ENCODER_ERROR,
        CANSimple::MSG_GET_SENSORLESS_ERROR,CANSimple::MSG_GET_ENCODER_ESTIMATES,CANSimple::MSG_GET_ENCODER_COUNT,
        CANSimple::MSG_GET_IQ,CANSimple::MSG_GET_SENSORLESS_ESTIMATES,CANSimple::MSG_GET_BUS_VOLTAGE_CURRENT,
        CANSimple::MSG_GET_ADC_VOLTAGE,CANSimple::MSG_GET_CONTROLLER_ERROR};
    const uint32_t writes[]={CANSimple::MSG_SET_AXIS_NODE_ID,CANSimple::MSG_SET_AXIS_REQUESTED_STATE,
        CANSimple::MSG_SET_AXIS_STARTUP_CONFIG,CANSimple::MSG_SET_CONTROLLER_MODES,CANSimple::MSG_SET_INPUT_POS,
        CANSimple::MSG_SET_INPUT_VEL,CANSimple::MSG_SET_INPUT_TORQUE,CANSimple::MSG_SET_LIMITS,
        CANSimple::MSG_START_ANTICOGGING,CANSimple::MSG_SET_TRAJ_VEL_LIMIT,CANSimple::MSG_SET_TRAJ_ACCEL_LIMITS,
        CANSimple::MSG_SET_TRAJ_INERTIA,CANSimple::MSG_RESET_ODRIVE,CANSimple::MSG_CLEAR_ERRORS,
        CANSimple::MSG_SET_LINEAR_COUNT,CANSimple::MSG_SET_POS_GAIN,CANSimple::MSG_SET_VEL_GAINS};
    for(Phase phase:{Phase::Native,Phase::Running,Phase::Paused}) {
        for(uint32_t command:reads) {
            prepare_phase(phase); CapturingCan sink; CANSimple can(&sink);
            can_Message_t message; message.id=(7u<<5)|command; message.rtr=true;
            const auto before=mutation_snapshot(); can.handle_can_message(message);
            CHECK(sink.sent.size()==1 && mutation_snapshot()==before && rp::native_rejected==0);
        }
        for(uint32_t command:writes) {
            prepare_phase(phase); CapturingCan sink; CANSimple can(&sink);
            const auto before=mutation_snapshot(); can.handle_can_message(request(command));
            if(phase!=Phase::Native) {
                CHECK(mutation_snapshot()==before && rp::native_rejected==1);
            } else {
                CHECK(axes[0].watchdog_feeds==1 && rp::native_rejected==0);
                switch(command) {
                    case CANSimple::MSG_SET_AXIS_NODE_ID: CHECK(axes[0].config_.can.node_id==42); break;
                    case CANSimple::MSG_SET_AXIS_REQUESTED_STATE: CHECK(axes[0].requested_state_==Axis::AXIS_STATE_MOTOR_CALIBRATION); break;
                    case CANSimple::MSG_SET_AXIS_STARTUP_CONFIG: CHECK(!axes[0].config_.startup_closed_loop_control); break; // firmware callback is intentionally empty
                    case CANSimple::MSG_SET_CONTROLLER_MODES: CHECK(controllers[0].mode_updates==1); break;
                    case CANSimple::MSG_SET_INPUT_POS: CHECK(controllers[0].input_pos_==2.5f && controllers[0].input_updates==1); break;
                    case CANSimple::MSG_SET_INPUT_VEL: CHECK(controllers[0].input_vel_==2.5f && controllers[0].input_torque_==1.25f); break;
                    case CANSimple::MSG_SET_INPUT_TORQUE: CHECK(controllers[0].input_torque_==2.5f); break;
                    case CANSimple::MSG_SET_LIMITS: CHECK(controllers[0].config_.vel_limit==2.5f && motors[0].config_.current_lim==1.25f); break;
                    case CANSimple::MSG_START_ANTICOGGING: CHECK(controllers[0].calibrations==1); break;
                    case CANSimple::MSG_SET_TRAJ_VEL_LIMIT: CHECK(axes[0].trap_traj_.config_.vel_limit==2.5f); break;
                    case CANSimple::MSG_SET_TRAJ_ACCEL_LIMITS: CHECK(axes[0].trap_traj_.config_.accel_limit==2.5f && axes[0].trap_traj_.config_.decel_limit==1.25f); break;
                    case CANSimple::MSG_SET_TRAJ_INERTIA: CHECK(controllers[0].config_.inertia==2.5f); break;
                    case CANSimple::MSG_RESET_ODRIVE: CHECK(resets==1); break;
                    case CANSimple::MSG_CLEAR_ERRORS: CHECK(odrv.clears==1); break;
                    case CANSimple::MSG_SET_LINEAR_COUNT: CHECK(encoders[0].shadow_count_==42); break;
                    case CANSimple::MSG_SET_POS_GAIN: CHECK(controllers[0].config_.pos_gain==2.5f); break;
                    case CANSimple::MSG_SET_VEL_GAINS: CHECK(controllers[0].config_.vel_gain==2.5f && controllers[0].config_.vel_integrator_gain==1.25f); break;
                }
            }
        }
    }
    for(Phase phase:{Phase::Native,Phase::Running,Phase::Paused}) {
        prepare_phase(phase); CapturingCan sink; CANSimple can(&sink); motors[0].is_armed_=true;
        can.handle_can_message(request(CANSimple::MSG_ODRIVE_ESTOP));
        CHECK(!motors[0].is_armed_ && (rp::latched&rehab::FaultStop));
        if(phase==Phase::Native)continue;
        prepare_phase(phase); motors[0].is_armed_=true;
        auto message=request(CANSimple::MSG_SET_AXIS_REQUESTED_STATE);
        can_setSignal<int32_t>(message,Axis::AXIS_STATE_IDLE,0,32,true); can.handle_can_message(message);
        CHECK(!motors[0].is_armed_ && (rp::latched&rehab::FaultStop) && axes[0].requested_state_==Axis::AXIS_STATE_IDLE);
    }
}
static void test_ascii_dispatch() {
    const char* writes[]={"p 0 2.5 1.25 0.5","q 0 2.5 3 4","v 0 2.5 0.5","c 0 2.5","t 0 2.5",
        "w axis0.controller.input_torque 2.5","w axis0.motor.config.current_lim 20",
        "w axis0.requested_state 4","ss","se","sr","sc","u 0","esl 0 42"};
    for(Phase phase:{Phase::Native,Phase::Running,Phase::Paused}) {
        for(const char* command:writes) {
            prepare_phase(phase); CapturingSink sink; AsciiProtocol protocol(nullptr,&sink);
            const auto before=mutation_snapshot(); ascii(protocol,command);
            if(phase==Phase::Native) CHECK(mutation_snapshot()!=before && rp::native_rejected==0);
            else CHECK(mutation_snapshot()==before && rp::native_rejected==1 && sink.text.find("control busy")!=std::string::npos);
        }
        for(const char* command:{"r axis0.controller.input_torque","f 0","h","i"}) {
            prepare_phase(phase); CapturingSink sink; AsciiProtocol protocol(nullptr,&sink);
            const auto before=mutation_snapshot(); ascii(protocol,command);
            CHECK(!sink.text.empty() && mutation_snapshot()==before && rp::native_rejected==0);
        }
        if(phase!=Phase::Native) {
            prepare_phase(phase); CapturingSink sink; AsciiProtocol protocol(nullptr,&sink); motors[0].is_armed_=true;
            ascii(protocol,"w axis0.requested_state 1");
            CHECK((rp::latched&rehab::FaultStop) && !motors[0].is_armed_ && sink.text.find("stopped")!=std::string::npos);
        }
    }
    prepare_phase(Phase::Native); CapturingSink sink; AsciiProtocol protocol(nullptr,&sink);
    ascii(protocol,"w axis0.controller.input_torque");
    CHECK(sink.text.find("invalid command format")!=std::string::npos && controllers[0].input_torque_==0);
    ascii(protocol,"c 1 7"); CHECK(controllers[0].input_torque_==0); // no axis1
}
static void test_generated_fibre() {
    uint8_t empty[1]{}; cbufptr_t input{empty,size_t(0)}; bufptr_t output{empty,size_t(0)};
    for(Phase phase:{Phase::Native,Phase::Running,Phase::Paused}) {
        prepare_phase(phase);
        const std::vector<std::function<bool()>> functions={
            [&]{bool success=false; return odrive_save_configuration(&odrv,&success,&input,&output);},
            [&]{return odrive_erase_configuration(&odrv,&input,&output);},
            [&]{return odrive_reboot(&odrv,&input,&output);},
            [&]{return odrive_enter_dfu_mode(&odrv,&input,&output);},
            [&]{return odrive_clear_errors(&odrv,&input,&output);},
            [&]{return odrive_controller_start_anticogging_calibration(&controllers[0],&input,&output);},
            [&]{return odrive_controller_move_incremental(&controllers[0],2.5f,true,&input,&output);},
            [&]{return odrive_encoder_set_linear_count(&encoders[0],42,&input,&output);},
            [&]{return odrive_axis_watchdog_feed(&axes[0],&input,&output);},
        };
        for(const auto& function:functions) {
            const auto before=mutation_snapshot(); const unsigned rejected=rp::native_rejected;
            const bool accepted=function();
            if(phase==Phase::Native)CHECK(accepted && mutation_snapshot()!=before);
            else CHECK(!accepted && mutation_snapshot()==before && rp::native_rejected==rejected+1);
        }
        float old=-1;
        const unsigned rejected=rp::native_rejected;
        CHECK(fibre_property_float32_readwrite_exchange(Property<float>{&controllers[0].input_torque_},2.5f,&old,&input,&output));
        CHECK(controllers[0].input_torque_==(phase==Phase::Native?2.5f:0.0f));
        CHECK(rp::native_rejected==rejected+(phase==Phase::Native?0:1));
        const unsigned after_write=rp::native_rejected;
        CHECK(fibre_property_float32_readwrite_exchange(Property<float>{&controllers[0].input_torque_},std::nullopt,&old,&input,&output));
        CHECK(old==controllers[0].input_torque_ && rp::native_rejected==after_write);

        // Actual generated argument properties and actual generated exchange
        // stubs. The production endpoint's diagnostic-copy call is verified by
        // the source manifest; the full endpoint dispatcher is not mocked here.
        uint32_t old_word=0, word=0; int32_t old_signed=0; uint8_t old_byte=0; float result=0;
        CHECK(fibre_property_uint32_readwrite_exchange(ODriveIntf::get_get_adc_voltage_in_gpio_(&odrv).diagnostic_argument(),7u,&old_word,&input,&output));
        CHECK(odrive_get_adc_voltage(&odrv,odrv.get_adc_voltage_in_gpio_,&result,&input,&output));
        CHECK(odrv.last_adc==7 && std::fabs(result-.7f)<1e-6f);
        CHECK(fibre_property_int32_readwrite_exchange(ODriveIntf::get_get_interrupt_status_in_irqn_(&odrv).diagnostic_argument(),-4,&old_signed,&input,&output));
        CHECK(odrive_get_interrupt_status(&odrv,odrv.get_interrupt_status_in_irqn_,&word,&input,&output));
        CHECK(odrv.last_irq==-4 && word==uint32_t(-4));
        CHECK(fibre_property_uint8_readwrite_exchange(ODriveIntf::get_get_dma_status_in_stream_num_(&odrv).diagnostic_argument(),uint8_t(3),&old_byte,&input,&output));
        CHECK(odrive_get_dma_status(&odrv,odrv.get_dma_status_in_stream_num_,&word,&input,&output));
        CHECK(odrv.last_dma==3 && word==3);
        CHECK(fibre_property_uint32_readwrite_exchange(ODriveIntf::OscilloscopeIntf::get_get_val_in_index_(&odrv.oscilloscope_).diagnostic_argument(),12u,&old_word,&input,&output));
        CHECK(odrive_oscilloscope_get_val(&odrv.oscilloscope_,odrv.oscilloscope_.get_val_in_index_,&result,&input,&output));
        CHECK(odrv.oscilloscope_.last_index==12 && result==12);
        CHECK(rp::native_rejected==after_write);
        if(phase!=Phase::Native) {
            Axis::AxisState previous{}; motors[0].is_armed_=true;
            CHECK(fibre_property_odrive_axis_axis_state_readwrite_exchange(Property<Axis::AxisState>{&axes[0].requested_state_},Axis::AXIS_STATE_IDLE,&previous,&input,&output));
            CHECK(!motors[0].is_armed_ && (rp::latched&rehab::FaultStop));
        }
    }
}
static void test_pwm_and_analog() {
    for(Phase phase:{Phase::Native,Phase::Running,Phase::Paused}) {
        prepare_phase(phase); auto& map=odrv.config_.pwm_mappings[0];
        map.endpoint={fibre::json_crc_,1}; map.min=-2; map.max=6;
        handle_pulse(0,1500*84);
        CHECK(controllers[0].input_torque_==(phase==Phase::Native?2.0f:0.0f));
        if(phase!=Phase::Native)CHECK(rp::native_rejected==1);
        PWMMapping_t analog; analog.endpoint={fibre::json_crc_,2}; analog.min=10; analog.max=30;
        update_analog_endpoint(&analog,3);
        CHECK(motors[0].config_.current_lim==(phase==Phase::Native?20.0f:10.0f));
        if(phase!=Phase::Native)CHECK(rp::native_rejected==2);
    }
    prepare_phase(Phase::Native); auto& map=odrv.config_.pwm_mappings[0];
    map.endpoint={fibre::json_crc_,1}; map.min=-2; map.max=6;
    handle_pulse(0,499*84); CHECK(controllers[0].input_torque_==0);
    handle_pulse(0,500*84); CHECK(controllers[0].input_torque_==-2);
    handle_pulse(0,2500*84); CHECK(controllers[0].input_torque_==6);
    CHECK(!fibre::set_endpoint_from_float({uint16_t(fibre::json_crc_^1),1},17));
    CHECK(controllers[0].input_torque_==6);
}
static void test_step_boundaries() {
    prepare_phase(Phase::Native); axes[0].set_step_dir_active(true);
    CHECK(axes[0].step_dir_active_ && subscriptions==1);
    const int64_t centers[]={0,1,-1,(1LL<<24),-(1LL<<24),(1LL<<33),-(1LL<<33),
        (1LL<<53),-(1LL<<53),std::numeric_limits<int64_t>::max()-8,std::numeric_limits<int64_t>::min()+8};
    for(int64_t center:centers)for(int delta=-3;delta<=3;++delta)for(bool forward:{false,true}) {
        const int64_t initial=center+delta;
        axes[0].steps_=initial; axes[0].dir_gpio_.write(forward);
        const unsigned updates=controllers[0].input_updates; axes[0].step_cb();
        CHECK(axes[0].steps_==initial+(forward?1:-1) && controllers[0].input_updates==updates+1);
        // Power-of-two step scale from the original long regression. This is a
        // reference projection, not a replacement native controller algorithm.
        const float projected=float(axes[0].steps_)/8192.0f;
        CHECK(projected==float(static_cast<long double>(axes[0].steps_)/8192.0L));
    }
    uint32_t seed=0x51e9ab37;
    for(unsigned i=0;i<10000;++i) {
        seed^=seed<<13; seed^=seed>>17; seed^=seed<<5;
        const int64_t initial=int64_t(int32_t(seed))+(i%2?(1LL<<33):-(1LL<<33));
        const bool forward=(seed&1)!=0; axes[0].steps_=initial; axes[0].dir_gpio_.write(forward);
        axes[0].step_cb(); CHECK(axes[0].steps_==initial+(forward?1:-1));
    }
    axes[0].steps_=(1LL<<24); axes[0].dir_gpio_.write(true); axes[0].step_cb();
    CHECK(axes[0].steps_==(1LL<<24)+1 && float(axes[0].steps_)==float(1LL<<24));
    axes[0].set_step_dir_active(false); const auto previous=axes[0].steps_; axes[0].step_cb();
    CHECK(!axes[0].step_dir_active_ && axes[0].steps_==previous && unsubscriptions==1);
    for(Phase phase:{Phase::Running,Phase::Paused}) {
        prepare_phase(phase); axes[0].set_step_dir_active(true);
        CHECK(!axes[0].step_dir_active_ && subscriptions==0 && unsubscriptions==1);
        axes[0].step_dir_active_=true; axes[0].steps_=100; // late edge after handoff
        axes[0].step_cb(); CHECK(axes[0].steps_==100 && controllers[0].input_updates==0 && rp::native_rejected==1);
    }
    prepare_phase(Phase::Native); subscribe_ok=false; axes[0].set_step_dir_active(true);
    CHECK(odrv.misconfigured_); // retained original subscribe-error behavior
}
static void test_clear_release_and_axis_admission() {
    prepare_phase(Phase::Running);
    CHECK(rp::axis_request_allowed(Axis::AXIS_STATE_IDLE));
    CHECK(rp::axis_request_allowed(Axis::AXIS_STATE_UNDEFINED));
    CHECK(!rp::axis_request_allowed(Axis::AXIS_STATE_MOTOR_CALIBRATION));
    CHECK(!rp::axis_request_allowed(Axis::AXIS_STATE_CLOSED_LOOP_CONTROL)); // hardware lock
    controllers[0].input_torque_=.8f; controllers[0].input_vel_=4;
    rehab::native_emergency_stop(); rp::engine.trip(rp::latched);
    axes[0].requested_state_=Axis::AXIS_STATE_UNDEFINED; // carrier: axis task consumed IDLE
    CHECK(rp::clear_control_faults(nullptr,11)==Result::Ok);
    CHECK(!rp::target.enable && !motors[0].is_armed_ && rp::owner_session==11);
    CHECK(rp::release_control(nullptr,11,rp::feedback)==Result::Ok);
    CHECK(!rp::owner_session && !rp::target.enable && !motors[0].is_armed_ && !rp::motor_may_arm());
    CHECK(controllers[0].input_torque_==0 && controllers[0].input_vel_==0);
    axes[0].set_step_dir_active(true); axes[0].dir_gpio_.write(false); axes[0].step_cb();
    CHECK(axes[0].step_dir_active_ && axes[0].steps_==-1 && controllers[0].input_updates==1);
    CapturingSink sink; AsciiProtocol protocol(nullptr,&sink); ascii(protocol,"c 0 1.5");
    CHECK(controllers[0].input_torque_==1.5f && !motors[0].is_armed_);
    CapturingCan can_sink; CANSimple can(&can_sink); can.handle_can_message(request(CANSimple::MSG_SET_INPUT_TORQUE));
    CHECK(controllers[0].input_torque_==2.5f && !motors[0].is_armed_);
}
int main() {
    struct Test {const char* name; void(*run)();} tests[]={
        {"CAN production dispatch and callbacks",test_can_all_commands},
        {"ASCII production process_line and handlers",test_ascii_dispatch},
        {"actual generated Fibre property and RPC stubs",test_generated_fibre},
        {"PWM and analog to actual endpoint write",test_pwm_and_analog},
        {"actual step ingress and bounded precision regression",test_step_boundaries},
        {"actual runtime clear release and axis admission",test_clear_release_and_axis_admission},
    };
    for(const auto& test:tests) {
        try {test.run(); std::printf("PASS %s\n",test.name);}
        catch(const TestFailure&) {std::printf("FAIL %s\n",test.name);}
    }
    std::printf("native ingress: %u checks, %u failed; production ingress bodies/stubs/runtime, host carriers\n",checks,failures);
    return failures?1:0;
}
