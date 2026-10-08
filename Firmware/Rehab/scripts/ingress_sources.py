#!/usr/bin/env python3
"""Extract complete production ingress functions for the host carrier harness.

This is source preparation/static verification only, never a compiler or runner.
No function body is rewritten. Sources, byte-exact function SHA256, line numbers,
build membership and call-site checks are emitted in manifest.json.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def source_name(path):
    path = pathlib.Path(path).resolve()
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def generated_headers(output, check_only):
    """Use the same real schema/generator/templates as Tup, in an isolated tree."""
    templates = {
        'interfaces.hpp': 'interfaces_template.j2',
        'function_stubs.hpp': 'function_stubs_template.j2',
        'endpoints.hpp': 'endpoints_template.j2',
        'type_info.hpp': 'type_info_template.j2',
    }
    inputs = [ROOT / 'Rehab/interface.py', ROOT / 'odrive-interface.yaml',
              ROOT / 'interface_generator_stub.py', ROOT / 'Tupfile.lua',
              ROOT.parent / 'tools/fibre-tools/interface_generator.py']
    inputs += [ROOT / 'fibre-cpp' / name for name in templates.values()]
    input_hashes = {source_name(path): digest(path.read_bytes()) for path in inputs}
    folder = output / 'autogen'
    schema = folder / 'rehab-interface.yaml'
    commands = [[sys.executable, '-B', str(ROOT / 'Rehab/interface.py'), '--output', str(schema)]]
    for header, template in templates.items():
        command = [sys.executable, '-B', str(ROOT / 'interface_generator_stub.py'),
                   '--definitions', str(schema), '--template', str(ROOT / 'fibre-cpp' / template),
                   '--output', str(folder / header)]
        if header == 'endpoints.hpp':
            command += ['--generate-endpoints', 'ODriveRehabV12']
        commands.append(command)
    if check_only:
        manifest_path = output / 'manifest.json'
        if not manifest_path.is_file():
            raise ValueError('No isolated ingress artifacts; first run without --check')
        previous = json.loads(manifest_path.read_text(encoding='utf-8'))
        if previous.get('generation_inputs') != input_hashes:
            raise ValueError('Schema/generator/template changed; regenerate ingress artifacts')
    else:
        folder.mkdir(parents=True, exist_ok=True)
        for command in commands:
            subprocess.run(command, cwd=ROOT, check=True)
    artifacts = [schema] + [folder / header for header in templates]
    hashes = {source_name(path): digest(path.read_bytes()) for path in artifacts}
    if check_only and previous.get('generated_headers') != hashes:
        raise ValueError('Independent generated headers changed; regenerate ingress artifacts')
    return folder, input_hashes, hashes, commands


def closing_brace(source, opening):
    depth, state, escaped, i = 0, 'code', False, opening
    while i < len(source):
        c = source[i]
        n = source[i + 1] if i + 1 < len(source) else ''
        if state == 'line':
            if c == '\n': state = 'code'
        elif state == 'comment':
            if c == '*' and n == '/': state = 'code'; i += 1
        elif state in ('"', "'"):
            if escaped: escaped = False
            elif c == '\\': escaped = True
            elif c == state: state = 'code'
        elif c == '/' and n == '/': state = 'line'; i += 1
        elif c == '/' and n == '*': state = 'comment'; i += 1
        elif c in ('"', "'"): state = c
        elif c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: return i + 1
        i += 1
    raise ValueError('Unclosed production function')


def extract(path, pattern):
    path = ROOT / path
    raw = path.read_bytes()
    # Preserve the actual line endings in each extracted body and its hash.
    source = raw.decode('utf-8')
    matches = list(re.finditer(pattern, source, re.M))
    if len(matches) != 1:
        raise ValueError(f'{path}: expected one function for {pattern!r}, found {len(matches)}')
    start = matches[0].start()
    opening = source.index('{', matches[0].end())
    end = closing_brace(source, opening)
    body = source[start:end]
    record = dict(source=source_name(path), source_sha256=digest(raw),
                  start_line=source.count('\n', 0, start) + 1,
                  end_line=source.count('\n', 0, end) + 1,
                  function_sha256=digest(body.encode('utf-8')),
                  signature=body[:body.index('{')].strip())
    return record, body


def prepare(output, check_only):
    output = pathlib.Path(output).resolve()
    tup = (ROOT / 'Tupfile.lua').read_text(encoding='utf-8')
    sources = ['communication/can/can_simple.cpp', 'communication/ascii_protocol.cpp',
               'MotorControl/axis.cpp', 'MotorControl/low_level.cpp', 'MotorControl/pwm_input.cpp']
    for path in sources:
        if repr(path) not in tup:
            raise ValueError('Not in firmware Tup source list: ' + path)
    if "'Rehab/platform/runtime.cpp'" not in tup:
        raise ValueError('Runtime is no longer in the V1.2 build')
    for template in ['function_stubs_template.j2', 'interfaces_template.j2', 'endpoints_template.j2', 'type_info_template.j2']:
        if template not in tup:
            raise ValueError('Fibre template no longer generated by firmware build: ' + template)

    # Never inspect or rewrite Firmware/autogen: an unrelated original-board ARM
    # build may currently own it. Host generation has its own output directory.
    generated, generation_inputs, generated_hashes, generation_commands = generated_headers(output, check_only)
    endpoints = (generated / 'endpoints.hpp').read_text(encoding='utf-8')
    if 'ODriveRehabV12' not in endpoints or '\\"axis1\\"' in endpoints:
        raise ValueError('Independent generated interface is not the V1.2 root')
    diagnostic_lines = [line for line in endpoints.splitlines() if '.diagnostic_argument()' in line]
    expected = ['::get_get_adc_voltage_in_', '::get_get_interrupt_status_in_',
                '::get_get_dma_status_in_', 'OscilloscopeIntf::get_get_val_in_']
    if len(diagnostic_lines) != 4 or any(sum(stem in line for line in diagnostic_lines) != 1 for stem in expected):
        raise ValueError('Unexpected diagnostic argument endpoint whitelist')

    checks = [
        ('communication/interface_usb.cpp', 'ascii_over_cdc.start()'),
        ('communication/interface_uart.cpp', 'ascii_over_uart.start()'),
        ('communication/ascii_protocol.cpp', 'process_line({rx_buf_, end_of_line})'),
        ('communication/can/can_simple.cpp', '((CANSimple*)ctx)->handle_can_message(msg)'),
        ('communication/can/can_simple.cpp', 'do_command(axis, msg)'),
        ('MotorControl/axis.cpp', 'reinterpret_cast<Axis*>(ctx)->step_cb()'),
        ('MotorControl/axis.cpp', 'step_gpio_.subscribe(true, false, step_cb_wrapper, this)'),
        ('MotorControl/axis.cpp', 'rehab::platform::axis_request_allowed(requested_state_)'),
        ('MotorControl/pwm_input.cpp', 'handle_pulse(channel, timestamp - last_timestamp[channel])'),
        ('MotorControl/pwm_input.cpp', 'fibre::set_endpoint_from_float('),
        ('MotorControl/low_level.cpp', 'update_analog_endpoint(map, i)'),
        ('MotorControl/low_level.cpp', 'fibre::set_endpoint_from_float('),
        ('Board/rehab_v12/board.cpp', 'pwm0_input.on_capture()'),
        ('communication/communication.cpp', 'autogen/endpoints.hpp'),
    ]
    for path, needle in checks:
        if needle not in (ROOT / path).read_text(encoding='utf-8'):
            raise ValueError(f'Firmware call-chain changed: {path}: {needle}')

    groups = {'can': [], 'ascii': [], 'axis': [], 'mapping': []}
    can_path = sources[0]
    can_text = (ROOT / can_path).read_text(encoding='utf-8')
    names = ['handle_can_message', 'do_command'] + re.findall(
        r'^(?:void|bool) CANSimple::(\w+_callback)\(', can_text, re.M)
    for name in names:
        groups['can'].append(extract(can_path, r'^(?:void|bool) CANSimple::' + name + r'\('))
    ascii_path = sources[1]
    ascii_text = (ROOT / ascii_path).read_text(encoding='utf-8')
    groups['ascii'].append(extract(ascii_path, r'^template<typename \.\.\. TArgs>\s*\nvoid AsciiProtocol::respond\('))
    names = ['process_line'] + re.findall(r'^void AsciiProtocol::(cmd_\w+)\(', ascii_text, re.M)
    for name in names:
        groups['ascii'].append(extract(ascii_path, r'^void AsciiProtocol::' + name + r'\('))
    groups['axis'].append(extract('MotorControl/axis.cpp', r'^static void step_cb_wrapper\('))
    for name in ['step_cb', 'set_step_dir_active']:
        groups['axis'].append(extract('MotorControl/axis.cpp', r'^void Axis::' + name + r'\('))
    groups['mapping'].append(extract(generated / 'endpoints.hpp', r'^bool set_endpoint_from_float\('))
    groups['mapping'].append(extract('MotorControl/pwm_input.cpp', r'^void handle_pulse\('))
    groups['mapping'].append(extract('MotorControl/low_level.cpp', r'^static void update_analog_endpoint\('))
    pwm_text = (ROOT / 'MotorControl/pwm_input.cpp').read_text(encoding='utf-8')
    pwm_defines = '\n'.join(line for line in pwm_text.splitlines() if line.startswith('#define '))
    records = [record for functions in groups.values() for record, _ in functions]
    headers = ['fibre-cpp/protocol.hpp', 'fibre-cpp/include/fibre/introspection.hpp',
               'Rehab/native_access.hpp', 'Rehab/platform/runtime.cpp',
               'Rehab/platform/isr_timing.cpp', 'Rehab/protocol/command_queue.hpp']
    manifest = dict(schema=1, functions=records, complete_function_count=len(records),
                    firmware_build='Tupfile.lua', call_chain_checks=checks,
                    diagnostic_argument_cases=diagnostic_lines,
                    generation_inputs=generation_inputs, generated_headers=generated_hashes,
                    generation_commands=generation_commands,
                    whole_file_inputs={path: digest((ROOT / path).read_bytes()) for path in headers},
                    boundaries=['HAL/RTOS and ODrive data carriers are mocked.',
                                'Fibre property lookup directory is a small test carrier, not the full endpoint dispatcher.',
                                'Function bodies, admission logic and generated RPC/property stubs are production code.',
                                'PWM tests enter complete handle_pulse; GPIO capture IRQ plumbing is checked structurally.',
                                'Step floating projections are reference checks; full native Controller::update is not simulated.'])
    for group, functions in groups.items():
        chunks = ['// Generated from production sources; do not edit.\n']
        if group == 'mapping': chunks.append(pwm_defines + '\n')
        for record, body in functions:
            if group == 'mapping' and 'set_endpoint_from_float' in record['signature']:
                chunks.append('namespace fibre {\n')
            chunks.append(f'#line {record["start_line"]} "{record["source"]}"\n' + body + '\n')
            if group == 'mapping' and 'set_endpoint_from_float' in record['signature']:
                chunks.append('}\n')
        generated_bytes = ''.join(chunks).encode('utf-8')
        if check_only:
            if (output / (group + '.inc')).read_bytes() != generated_bytes:
                raise ValueError('Extracted production functions are stale: ' + group)
        else:
            (output / (group + '.inc')).write_bytes(generated_bytes)
    if not check_only:
        (output / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Static ingress provenance: {len(records)} complete functions, {len(checks)} call-chain checks, 4 diagnostic argument cases')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, default=ROOT / 'build/rehab/ingress_generated')
    parser.add_argument('--check', action='store_true', help='Read/verify only; do not generate files.')
    args = parser.parse_args()
    prepare(args.output, args.check)


if __name__ == '__main__':
    main()
