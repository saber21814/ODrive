#pragma once
#include <autogen/interfaces.hpp>
#include "runtime.hpp"
namespace rehab { namespace platform {
class Diagnostics : public RehabStatusIntf {
public:
    uint32_t get_state() const { return diagnostic_word(0); }
    uint32_t get_mode() const { return diagnostic_word(1); }
    uint32_t get_faults() const { return diagnostic_word(2); }
    uint32_t get_owner() const { return diagnostic_word(3); }
    uint32_t get_epoch() const { return diagnostic_word(4); }
    uint32_t get_config_valid() const { return diagnostic_word(5); }
    uint32_t get_force_valid() const { return diagnostic_word(6); }
    uint32_t get_sample_age_us() const { return diagnostic_word(7); }
    uint32_t get_native_rejected() const { return diagnostic_word(8); }
    uint32_t get_native_writes() const { return diagnostic_word(9); }
    uint32_t get_hardware_locked() const { return diagnostic_word(10); }
    uint32_t get_stage() const { return diagnostic_word(11); }
    uint32_t get_hx711_enabled() const { return diagnostic_word(12); }
    uint32_t get_network_enabled() const { return diagnostic_word(13); }
    uint32_t get_command_rejected() const { return diagnostic_word(14); }
    uint32_t get_rx_overflow() const { return diagnostic_word(15); }
    uint32_t get_tx_overflow() const { return diagnostic_word(16); }
    uint32_t get_max_hook_cycles() const { return diagnostic_word(17); }
    uint32_t get_max_service_cycles() const { return diagnostic_word(18); }
    uint32_t get_safety_stack_free() const { return diagnostic_word(19); }
    uint32_t get_network_stack_free() const { return diagnostic_word(20); }
    uint32_t get_spi_timeouts() const { return diagnostic_word(21); }
    uint32_t get_spi_errors() const { return diagnostic_word(22); }
    uint32_t get_spi_rejected() const { return diagnostic_word(23); }
    uint32_t get_spi_highwater() const { return diagnostic_word(24); }
    uint32_t get_network_errors() const { return diagnostic_word(25); }
    uint32_t get_hx711_timing_faults() const { return diagnostic_word(26); }
    uint32_t get_log_dropped() const { return diagnostic_word(27); }
    uint32_t get_repetitions() const { return diagnostic_word(28); }
    uint32_t get_motor_armed() const { return diagnostic_word(29); }
    uint32_t get_pending_rx_bytes() const { return diagnostic_word(30); }
    uint32_t get_force_raw_bits() const { return diagnostic_word(31); }
    uint32_t get_force_sequence() const { return diagnostic_word(32); }
    uint32_t get_force_raw_age_us() const { return diagnostic_word(33); }
    uint32_t get_force_raw_valid() const { return diagnostic_word(34); }
    uint32_t get_spi_quarantined() const { return diagnostic_word(35); }
    uint32_t get_network_state() const { return diagnostic_word(36); }
    uint32_t get_network_last_error() const { return diagnostic_word(37); }
    uint32_t get_network_link() const { return diagnostic_word(38); }
    uint32_t get_network_connected() const { return diagnostic_word(39); }
    uint32_t get_network_chip_version() const { return diagnostic_word(40); }
    uint32_t get_network_reconnects() const { return diagnostic_word(41); }
    uint32_t get_rx_highwater_bytes() const { return diagnostic_word(42); }
    uint32_t get_last_native_rejection() const { return diagnostic_word(43); }
    uint32_t get_bad_crc() const { return diagnostic_word(44); }
    uint32_t get_bad_header() const { return diagnostic_word(45); }
    uint32_t get_partial_frame_expired() const { return diagnostic_word(46); }
    uint32_t get_uptime_ms() const { return diagnostic_word(47); }
    uint32_t get_command_pending() const { return diagnostic_word(48); }
    uint32_t get_command_highwater() const { return diagnostic_word(49); }
    uint32_t get_command_queue_full() const { return diagnostic_word(50); }
    uint32_t get_command_stale() const { return diagnostic_word(51); }
    uint32_t get_command_capacity() const { return diagnostic_word(52); }
    uint32_t get_isr_control_max_cycles() const { return diagnostic_word(53); }
    uint32_t get_isr_tim8_max_cycles() const { return diagnostic_word(54); }
    uint32_t get_isr_spi_tx_max_cycles() const { return diagnostic_word(55); }
    uint32_t get_isr_spi_rx_max_cycles() const { return diagnostic_word(56); }
    uint32_t get_isr_stop_max_cycles() const { return diagnostic_word(57); }
    uint32_t get_isr_control_count() const { return diagnostic_word(58); }
    uint32_t get_isr_tim8_count() const { return diagnostic_word(59); }
    uint32_t get_isr_spi_tx_count() const { return diagnostic_word(60); }
    uint32_t get_isr_spi_rx_count() const { return diagnostic_word(61); }
    uint32_t get_isr_stop_count() const { return diagnostic_word(62); }
#if defined(REHAB_V13)
    uint32_t get_board_revision() const { return diagnostic_word(63); }
    uint32_t get_network_spi_bus() const { return diagnostic_word(64); }
    uint32_t get_native_spi_bus() const { return diagnostic_word(65); }
    uint32_t get_native_spi_timeouts() const { return diagnostic_word(66); }
    uint32_t get_native_spi_errors() const { return diagnostic_word(67); }
    uint32_t get_native_spi_rejected() const { return diagnostic_word(68); }
    uint32_t get_native_spi_highwater() const { return diagnostic_word(69); }
    uint32_t get_native_spi_quarantined() const { return diagnostic_word(70); }
    uint32_t get_gate_ready() const { return diagnostic_word(71); }
    uint32_t get_gate_generation() const { return diagnostic_word(72); }
    uint32_t get_physical_stop() const { return diagnostic_word(73); }
    uint32_t get_brake_settled_path() const { return diagnostic_word(74); }
    uint32_t get_software_brake_enabled() const { return diagnostic_word(75); }
    uint32_t get_hardware_stop_brake_present() const { return diagnostic_word(76); }
    uint32_t get_isr_spi1_tx_max_cycles() const { return diagnostic_word(77); }
    uint32_t get_isr_spi1_rx_max_cycles() const { return diagnostic_word(78); }
    uint32_t get_isr_spi1_tx_count() const { return diagnostic_word(79); }
    uint32_t get_isr_spi1_rx_count() const { return diagnostic_word(80); }
    float get_aux_temperature_c() const { return diagnostic_float(10); }
#endif
    float get_length_m() const { return diagnostic_float(0); }
    float get_speed_m_s() const { return diagnostic_float(1); }
    float get_force_n() const { return diagnostic_float(2); }
    float get_target_torque_nm() const { return diagnostic_float(3); }
    float get_power_w() const { return diagnostic_float(4); }
    float get_positive_energy_j() const { return diagnostic_float(5); }
    float get_negative_energy_j() const { return diagnostic_float(6); }
    float get_rfd_n_s() const { return diagnostic_float(7); }
    float get_vbus_v() const { return diagnostic_float(8); }
    float get_fet_temperature_c() const { return diagnostic_float(9); }
};
}}
