#pragma once
#include <cstddef>
#include <cstdint>
#include "Rehab/native_access.hpp"
namespace rehab { namespace platform {
void early_init();
void start();
void fast_guard();
void control_hook();
void post_controller_hook();
bool motor_may_arm();
bool brake_may_arm();
void native_clear_faults();
bool axis_request_allowed(uint32_t state);
float fet_temperature_c(float adc_ratio);
uint32_t diagnostic_word(unsigned index);
float diagnostic_float(unsigned index);
// These calls only copy bytes/frames in short critical sections. The application
// task owns protocol parsing, command execution, sessions and replies.
void receive_bytes(unsigned transport,const uint8_t*,size_t);
size_t transmit_bytes(unsigned transport,uint8_t*,size_t);
void disconnected(unsigned transport);
void service_transports();
}}
