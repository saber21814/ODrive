#!/usr/bin/env python3
"""Generate a V1.2-only schema. Never change the legacy schema or host enums."""
import argparse, copy, pathlib, yaml
WORDS = ["state","mode","faults","owner","epoch","config_valid","force_valid","sample_age_us","native_rejected","native_writes","hardware_locked","stage","hx711_enabled","network_enabled","command_rejected","rx_overflow","tx_overflow","max_hook_cycles","max_service_cycles","safety_stack_free","network_stack_free","spi_timeouts","spi_errors","spi_rejected","spi_highwater","network_errors","hx711_timing_faults","log_dropped","repetitions","motor_armed","pending_rx_bytes","force_raw_bits","force_sequence","force_raw_age_us","force_raw_valid","spi_quarantined","network_state","network_last_error","network_link","network_connected","network_chip_version","network_reconnects","rx_highwater_bytes","last_native_rejection","bad_crc","bad_header","partial_frame_expired","uptime_ms","command_pending","command_highwater","command_queue_full","command_stale","command_capacity","isr_control_max_cycles","isr_tim8_max_cycles","isr_spi_tx_max_cycles","isr_spi_rx_max_cycles","isr_stop_max_cycles","isr_control_count","isr_tim8_count","isr_spi_tx_count","isr_spi_rx_count","isr_stop_count"]
FLOATS = ["length_m","speed_m_s","force_n","target_torque_nm","power_w","positive_energy_j","negative_energy_j","rfd_n_s","vbus_v","fet_temperature_c"]
V13_WORDS = ['board_revision','network_spi_bus','native_spi_bus','native_spi_timeouts',
    'native_spi_errors','native_spi_rejected','native_spi_highwater','native_spi_quarantined',
    'gate_ready','gate_generation','physical_stop','brake_settled_path',
    'software_brake_enabled','hardware_stop_brake_present','isr_spi1_tx_max_cycles',
    'isr_spi1_rx_max_cycles','isr_spi1_tx_count','isr_spi1_rx_count']
p=argparse.ArgumentParser();p.add_argument('--output',required=True)
p.add_argument('--board',choices=['v12','v13'],default='v12');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
data=yaml.safe_load((root/'odrive-interface.yaml').read_text(encoding='utf-8'))
intfs=data['interfaces']
custom=copy.deepcopy(intfs['ODrive3'])
custom['attributes'].pop('axis1')
custom['attributes']['rehab']={'type':'RehabStatus','c_name':'rehab_'}
intfs['ODriveRehabV12']=custom
attrs={n:{'type':'readonly uint32','c_getter':'get_'+n+'()'} for n in WORDS}
attrs.update({n:{'type':'readonly float32','c_getter':'get_'+n+'()'} for n in FLOATS})
# Append only after the complete existing property list. Legacy diagnostics and
# framed command IDs retain their order; V1.2 schema stays byte-for-byte stable.
if a.board=='v13':
    attrs.update({n:{'type':'readonly uint32','c_getter':'get_'+n+'()'} for n in V13_WORDS})
    attrs['aux_temperature_c']={'type':'readonly float32','c_getter':'get_aux_temperature_c()'}
intfs['RehabStatus']={'c_is_class':True,'attributes':attrs}
data['valuetypes']['ODrive.StreamProtocolType']['values']['Rehab']={'doc':'Exclusive framed rehabilitation protocol. No ASCII or stdout on this stream.'}
pathlib.Path(a.output).write_text(yaml.safe_dump(data,sort_keys=False,allow_unicode=True),encoding='utf-8')
