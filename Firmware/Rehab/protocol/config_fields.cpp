#include "config_fields.hpp"
#include <cmath>
#include <cstring>
#include <cstddef>
namespace rehab { namespace wire {
struct Field { size_t offset; bool integer; const char* name; };
static const Field fields[] = {
    {offsetof(Config, radius_m),false,"radius_m"},
    {offsetof(Config, motor_turns_per_drum_turn),false,"motor_turns_per_drum_turn"},
    {offsetof(Config, encoder_turns_per_motor_turn),false,"encoder_turns_per_motor_turn"},
    {offsetof(Config, direction),false,"direction"},
    {offsetof(Config, zero_turns),false,"zero_turns"},
    {offsetof(Config, efficiency),false,"efficiency"},
    {offsetof(Config, force_scale_n_per_count),false,"force_scale_n_per_count"},
    {offsetof(Config, force_offset_counts),false,"force_offset_counts"},
    {offsetof(Config, force_filter_hz),false,"force_filter_hz"},
    {offsetof(Config, min_length_m),false,"min_length_m"},
    {offsetof(Config, max_length_m),false,"max_length_m"},
    {offsetof(Config, max_speed_m_s),false,"max_speed_m_s"},
    {offsetof(Config, max_force_n),false,"max_force_n"},
    {offsetof(Config, max_torque_nm),false,"max_torque_nm"},
    {offsetof(Config, torque_slew_nm_s),false,"torque_slew_nm_s"},
    {offsetof(Config, force_kp),false,"force_kp"},
    {offsetof(Config, force_ki),false,"force_ki"},
    {offsetof(Config, integrator_limit_nm),false,"integrator_limit_nm"},
    {offsetof(Config, concentric_force_n),false,"concentric_force_n"},
    {offsetof(Config, eccentric_force_n),false,"eccentric_force_n"},
    {offsetof(Config, speed_limit_m_s),false,"speed_limit_m_s"},
    {offsetof(Config, elastic_n_m),false,"elastic_n_m"},
    {offsetof(Config, water_n_s2_m2),false,"water_n_s2_m2"},
    {offsetof(Config, phase_speed_hysteresis_m_s),false,"phase_speed_hysteresis_m_s"},
    {offsetof(Config, phase_min_distance_m),false,"phase_min_distance_m"},
    {offsetof(Config, phase_dwell_ms),true,"phase_dwell_ms"},
    {offsetof(Config, heartbeat_timeout_ms),true,"heartbeat_timeout_ms"},
    {offsetof(Config, sensor_timeout_ms),true,"sensor_timeout_ms"},
    {offsetof(Config, target_timeout_ms),true,"target_timeout_ms"},
    {offsetof(Config, stop_timeout_ms),true,"stop_timeout_ms"},
    {offsetof(Config, return_speed_m_s),false,"return_speed_m_s"},
    {offsetof(Config, return_timeout_ms),true,"return_timeout_ms"},
    {offsetof(Config, fitness_duration_ms),true,"fitness_duration_ms"},
    {offsetof(Config, rfd_window_ms),true,"rfd_window_ms"},
    {offsetof(Config, friction_nm),false,"friction_nm"},
    {offsetof(Config, viscous_nm_s),false,"viscous_nm_s"},
    {offsetof(Config, inertia_kg_m2),false,"inertia_kg_m2"},
    {offsetof(Config, compensation_limit_nm),false,"compensation_limit_nm"},
    {offsetof(Config, flags),true,"flags"},
};
size_t config_field_count() { return sizeof(fields)/sizeof(fields[0]); }
const char* config_field_name(uint16_t id) { return id<config_field_count()?fields[id].name:nullptr; }
Result set_config_field(Config& c,uint16_t id,uint16_t type,uint32_t bits) {
    if(id>=config_field_count() || type!=unsigned(fields[id].integer)) return Result::InvalidConfig;
    if(!type) { float v; std::memcpy(&v,&bits,4);if(!std::isfinite(v)) return Result::InvalidConfig; }
    std::memcpy(reinterpret_cast<uint8_t*>(&c)+fields[id].offset,&bits,4);return Result::Ok;
}
bool get_config_field(const Config& c,uint16_t id,uint16_t& type,uint32_t& bits) {
    if(id>=config_field_count())return false;
    type=fields[id].integer;std::memcpy(&bits,reinterpret_cast<const uint8_t*>(&c)+fields[id].offset,4);return true;
}
}}

