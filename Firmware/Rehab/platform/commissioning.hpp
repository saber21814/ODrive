#pragma once
// These are REVIEWED HARDWARE declarations, not motor defaults. Keep zero/false
// until the board issue list and commissioning records have been resolved.
// They intentionally cannot be altered through an arbitrary Fibre property.
#ifndef REHAB_REAL_MOTOR
#define REHAB_REAL_MOTOR 0
#endif
namespace rehab { namespace commissioning {
constexpr bool brake_hs_connection_verified = false;
constexpr bool brake_resistor_and_regen_verified = false;
constexpr bool current_polarity_verified = false;
constexpr bool motor_encoder_parameters_verified = false;
constexpr bool timing_verified = false;
constexpr int stop_active_level = -1; // Must be 0 or 1; NC normally implies 1.
constexpr float ntc_beta = 0.0f; // Datasheet B25/85 or equivalent validated fit.
constexpr float ntc_r25 = 10000.0f;
constexpr float inverter_derate_c = 0.0f;
constexpr float inverter_trip_c = 0.0f;
constexpr float bus_min_v = 0.0f, bus_max_v = 0.0f;
constexpr bool verified = brake_hs_connection_verified && brake_resistor_and_regen_verified
    && current_polarity_verified && motor_encoder_parameters_verified && timing_verified
    && (stop_active_level==0 || stop_active_level==1) && ntc_beta>0.0f
    && inverter_trip_c>inverter_derate_c && inverter_derate_c>0.0f
    && bus_min_v>0.0f && bus_max_v>bus_min_v;
constexpr bool may_energize = REHAB_REAL_MOTOR && verified;
}}
