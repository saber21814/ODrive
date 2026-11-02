#!/usr/bin/env python3
"""Inspect actual ARM ELF sections, enabled modules, SRAM DMA placement and interfaces."""
import argparse
import json
import pathlib
import re
import subprocess
import sys
from build import source_digest

FW = pathlib.Path(__file__).resolve().parents[2]
OUT = FW/'build/rehab'
PROFILES = ['baseline', 'native', 'hx711', 'network', 'peripherals', 'protection', 'training', 'statistics', 'full']


def inspect(name,board='v12'):
    folder = (OUT/'v13' if board=='v13' and name!='baseline' else OUT)/name
    elf = folder/'ODriveFirmware.elf'
    sections = subprocess.check_output(['arm-none-eabi-size', '-A', str(elf)], text=True)
    sec = {}
    for line in sections.splitlines():
        match = re.match(r'(\.[\w.]+)\s+(\d+)\s+(\d+)', line)
        if match: sec[match[1]] = {'size': int(match[2]), 'address': int(match[3])}
    symbols = subprocess.check_output(['arm-none-eabi-nm', '-S', '-C', str(elf)], text=True)
    interface = (folder/'endpoints.hpp').read_text(encoding='utf-8')
    v12 = name != 'baseline'
    errors = []
    if v12 and re.search(r'\baxis1\b', interface): errors.append('Single-axis endpoint table contains axis1')
    if not v12 and 'axis1' not in interface: errors.append('Original interface lost axis1')
    hx = name in ('hx711', 'peripherals', 'protection', 'training', 'statistics', 'full')
    network = name in ('network', 'peripherals', 'protection', 'training', 'statistics', 'full')
    if ('rehab::drivers::Hx711::poll' in symbols) != hx: errors.append('HX711 feature/symbol mismatch')
    if ('network_main(' in symbols) != network: errors.append('Network task feature/symbol mismatch')
    if not v12 and 'rehab::platform::' in symbols: errors.append('Rehab runtime linked into original board')
    for function in ('rehab_isr_begin','rehab_isr_end','rehab_isr_max_cycles','rehab_isr_count'):
        if bool(re.search(r'\b'+function+r'$',symbols,re.M)) != v12:
            errors.append('ISR telemetry feature/symbol mismatch: '+function)
    command_queue = re.search(r'^([0-9a-f]+)\s+([0-9a-f]+)\s+b\s+rehab::platform::\(anonymous namespace\)::commands$',symbols,re.M)
    if bool(command_queue) != network: errors.append('Structured command queue/protocol feature mismatch')
    command_bytes=int(command_queue[2],16) if command_queue else 0
    dma = []
    for line in symbols.splitlines():
        match = re.match(r'([0-9a-f]+)\s+([0-9a-f]+)\s+[bBdD]\s+(.+)', line)
        if not match: continue
        address, size, label = int(match[1], 16), int(match[2], 16), match[3]
        if v12 and (label in ('spi3_arbiter','spi1_arbiter') or 'general_adc_samples' in label):
            dma.append({'symbol': label, 'address': hex(address), 'bytes': size})
            if not (0x20000000 <= address and address+size <= 0x20020000): errors.append('DMA storage outside SRAM: '+label)
    if v12 and not dma: errors.append('Missing DMA SRAM evidence')
    if v12 and board=='v13' and not any(d['symbol']=='spi1_arbiter' for d in dma): errors.append('Missing SPI1 DMA SRAM evidence')
    # init() may be fully inlined into Motor::setup at -O2. The actual wrapper
    # vtable and virtual readiness check remain linked in every V1.3 profile.
    if v12 and board=='v13' and ('vtable for V13GateDriver' not in symbols or 'V13GateDriver::is_ready()' not in symbols): errors.append('Missing V1.3 driver wrapper')
    if v12 and board=='v12' and ('spi1_arbiter' in symbols or 'V13GateDriver::' in symbols): errors.append('V1.3 board leaked into V1.2')
    # GNU size includes NOLOAD heap/stack reservations in bss. Keep banks separate.
    ram_sections = {key: value for key, value in sec.items() if 0x20000000 <= value['address'] < 0x20020000}
    sram = sum(value['size'] for value in ram_sections.values())
    ccm = sum(value['size'] for value in sec.values() if 0x10000000 <= value['address'] < 0x10010000)
    flash = sum(value['size'] for value in sec.values() if 0x08000000 <= value['address'] < 0x080c0000) + sec.get('.data', {}).get('size', 0)
    if sram > 128*1024 or ccm > 64*1024 or flash > 768*1024: errors.append('Memory budget exceeded')
    build = json.loads((folder/'build.json').read_text())
    if build.get('status')!='passed': errors.append('Build was not successful; old artifacts are not evidence')
    if build.get('board')!=('original' if name=='baseline' else board): errors.append('Artifact board mismatch')
    if build.get('source_sha256')!=source_digest(): errors.append('Artifact source digest is stale')
    return {'profile': name, 'flash_bytes': flash, 'sram_reserved_bytes': sram,
        'sram_unreserved_bytes': 128*1024-sram, 'ccm_rtos_heap_bytes': ccm,
        'safety_task_stack_bytes': 6144 if v12 else 0, 'network_task_stack_bytes': 3072 if network else 0,
        'structured_command_queue_bytes':command_bytes,'isr_telemetry_linked':v12,
        'dma': dma, 'sections': sec, 'errors': errors, 'build': build}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('profile', choices=PROFILES+['all'], default='all', nargs='?')
    p.add_argument('--board',choices=['v12','v13'],default='v12')
    a=p.parse_args()
    reports=[inspect(name,a.board) for name in PROFILES if a.profile in ('all',name)]
    destination=OUT/'v13' if a.board=='v13' else OUT
    destination.mkdir(parents=True,exist_ok=True)
    (destination/'resources.json').write_text(json.dumps(reports, indent=2)+'\n', encoding='utf-8')
    lines=['# ARM resource report', '', 'Generated by `Firmware/Rehab/scripts/resources.py`; actual ELF sections, in bytes. CCM contains the statically reserved FreeRTOS heap.', '',
        'Source SHA256: `'+reports[0]['build'].get('source_sha256','unknown')+'`.', '',
        '| Profile | Flash load | SRAM reserved | SRAM unreserved | CCM heap | Safety stack | Network stack | Checks |',
        '|---|---:|---:|---:|---:|---:|---:|---|']
    for r in reports:
        lines.append('| '+ ' | '.join(str(r[k]) for k in ('profile','flash_bytes','sram_reserved_bytes','sram_unreserved_bytes','ccm_rtos_heap_bytes','safety_task_stack_bytes','network_task_stack_bytes'))+' | '+('FAIL: '+', '.join(r['errors']) if r['errors'] else 'PASS')+' |')
    lines += ['', 'SRAM reserved includes linked .data/.bss and the linker C heap/main-stack reservation; task stacks are allocated inside the CCM FreeRTOS heap and must not be added twice.', '', 'Live task stack minima, IRQ WCET, DMA timing, free heap after startup and physical throughput require board measurement. No live measurements are claimed.']
    (destination/'resources.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    if a.profile=='all' and not any(r['errors'] for r in reports):
        docs=FW/'Rehab/docs'
        docs.mkdir(parents=True,exist_ok=True)
        (docs/('resources_v13.md' if a.board=='v13' else 'resources.md')).write_text('\n'.join(lines)+'\n',encoding='utf-8')
    print('\n'.join(lines))
    return int(any(r['errors'] for r in reports))


if __name__=='__main__': sys.exit(main())
