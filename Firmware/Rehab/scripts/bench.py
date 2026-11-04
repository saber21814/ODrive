#!/usr/bin/env python3
"""Explicit, single-phase V1.3 TCP bench client. Default command is read-only status."""
import argparse
import datetime
import json
import math
from pathlib import Path
import socket
import struct
import sys
import time
import zlib

HEADER = struct.Struct('<2sBBHHII')
WORDS = ('version enabled active stage state reason epoch remaining_ms heartbeat_age_ms '
         'axis_state armed motor_calibrated encoder_ready motor_passed encoder_passed '
         'system_error axis_error motor_error_lo motor_error_hi encoder_error controller_error '
         'encoder_count_bits encoder_cpr gate_ready physical_stop adc_packed adc_age_ms adc_errors '
         'network_state network_link spi1_errors spi1_timeouts spi3_errors spi3_timeouts hx_sequence').split()
FLOATS = ('vbus position_turns velocity_turns_s iq id phase_resistance phase_inductance '
          'offset_b offset_c fet_temperature_c aux_temperature_c argument').split()
STATUS_BYTES = 4 * (1 + len(WORDS) + len(FLOATS))
PHASES = {'motor-cal': (1, 8), 'encoder-cal': (2, 15), 'zero': (3, 5),
          'velocity': (4, 10), 'position': (5, 10), 'open-loop': (6, 1)}


class ProtocolError(RuntimeError):
    pass


class CommandRejected(ProtocolError):
    def __init__(self, command, result):
        super().__init__('Command %d rejected: result=%d' % (command, result))
        self.command, self.result = command, result


def frame(command, session, sequence, payload=b'', flags=0):
    if len(payload) > 256:
        raise ValueError('Payload too long')
    raw = HEADER.pack(b'RH', 1, flags, len(payload), command, session, sequence) + payload
    return raw + struct.pack('<I', zlib.crc32(raw))


def parse_status(payload):
    if len(payload) != STATUS_BYTES:
        raise ProtocolError('Bench status length must be 192 bytes')
    result = struct.unpack_from('<I', payload)[0]
    if result:
        raise ProtocolError('Bench result=%d' % result)
    values = struct.unpack_from('<35I12f', payload, 4)
    status = dict(zip(WORDS + FLOATS, values))
    if status['version'] != 1:
        raise ProtocolError('Unsupported bench status version')
    raw = status['encoder_count_bits']
    status['encoder_count'] = raw - (1 << 32) if raw & (1 << 31) else raw
    return status


class Client:
    def __init__(self, connection, log=lambda *_args, **_kwargs: None, timeout=2.0):
        self.connection = connection
        self.log = log
        self.timeout = timeout
        self.session = 0
        self.sequence = 0

    def _remaining(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('Frame deadline exceeded')
        self.connection.settimeout(remaining)

    def _read(self, count, deadline):
        result = bytearray()
        while len(result) < count:
            self._remaining(deadline)
            chunk = self.connection.recv(count - len(result))
            if not chunk:
                raise ConnectionError('TCP peer closed the connection')
            result.extend(chunk)
        return bytes(result)

    def request(self, command, payload=b''):
        if self.sequence == 0xffffffff:
            raise ProtocolError('Sequence exhausted; no automatic reconnect')
        self.sequence += 1
        sequence = self.sequence
        raw = frame(command, self.session, sequence, payload)
        deadline = time.monotonic() + self.timeout
        self.log('tx', raw_hex=raw.hex(), command=command, sequence=sequence)
        self._remaining(deadline)
        self.connection.sendall(raw)
        header = self._read(HEADER.size, deadline)
        magic, version, flags, length, cmd, session, seq = HEADER.unpack(header)
        if magic != b'RH' or version != 1 or flags != 1 or length < 4 or length > 256:
            raise ProtocolError('Invalid response header')
        tail = self._read(length + 4, deadline)
        response = header + tail
        self.log('rx', raw_hex=response.hex(), command=cmd, sequence=seq)
        if zlib.crc32(response[:-4]) != struct.unpack('<I', response[-4:])[0]:
            raise ProtocolError('Bad response CRC')
        if cmd != command or seq != sequence:
            raise ProtocolError('Response command/sequence mismatch')
        if command == 1:
            if not session:
                raise ProtocolError('Hello returned zero session')
        elif not self.session or session != self.session:
            raise ProtocolError('Response session mismatch')
        payload = tail[:-4]
        result = struct.unpack_from('<I', payload)[0]
        if result:
            raise CommandRejected(command, result)
        if command == 1:
            if len(payload) != 16 or struct.unpack('<4I', payload) != (0, 1, 256, 0x1201):
                raise ProtocolError('Unexpected Hello interface')
            self.session = session
        return payload

    def hello(self):
        if self.session:
            raise ProtocolError('No implicit Hello replacement')
        self.request(1)

    def bench(self, command=25, payload=b''):
        status = parse_status(self.request(command, payload))
        metadata = ({'encoder_measured': False, 'current_frame': 'commanded_electrical_frame',
                     'argument_unit': 'electrical_rad_s'} if status['stage'] == 6 else {})
        self.log('status', command=command, **status, **metadata)
        return status


def preconditions(status, stage):
    if not status['enabled'] or status['active'] or status['armed'] or status['axis_state'] != 1:
        raise ProtocolError('Bench disabled, occupied, armed, or not idle')
    errors = ('system_error', 'axis_error', 'motor_error_lo', 'motor_error_hi',
              'encoder_error', 'controller_error')
    if any(status[k] for k in errors) or status['state'] == 5:
        raise ProtocolError('Fault/reason remains; inspect and explicitly clear first')
    if status['physical_stop'] or not status['gate_ready']:
        raise ProtocolError('STOP or gate prerequisite failed')
    if status['adc_age_ms'] > 10 or status['adc_packed'] & 0x8f000000 != 0x87000000:
        raise ProtocolError('ADC prerequisite failed')
    if not math.isfinite(status['vbus']) or not 22 <= status['vbus'] <= 26:
        raise ProtocolError('Invalid or out-of-range vbus')
    for key in ('fet_temperature_c', 'aux_temperature_c'):
        if not math.isfinite(status[key]) or not -10 <= status[key] < 50:
            raise ProtocolError('Invalid or out-of-range ' + key)
    if stage not in (1, 6):
        if status['encoder_cpr'] != 4000:
            raise ProtocolError('CPR prerequisite failed')
        if not all(math.isfinite(status[k]) for k in ('position_turns', 'velocity_turns_s')):
            raise ProtocolError('Invalid encoder telemetry')
        if abs(status['velocity_turns_s']) > 0.02:
            raise ProtocolError('Motor is not stationary enough to start')
    if stage >= 2 and not status['motor_passed']:
        raise ProtocolError('Motor calibration has not passed in this boot')
    if stage in (3, 4, 5) and not status['encoder_passed']:
        raise ProtocolError('Encoder calibration has not passed in this boot')
    if stage == 6 and not status['motor_calibrated']:
        raise ProtocolError('Native motor calibration is not valid')


def duration_ms(seconds):
    if not math.isfinite(seconds) or not 0.2 <= seconds <= 5.0:
        raise ValueError('--seconds must be finite within 0.2..5')
    milliseconds = seconds * 1000.0
    rounded = round(milliseconds)
    if not math.isclose(milliseconds, rounded, rel_tol=0.0, abs_tol=1e-9):
        raise ValueError('--seconds must specify an integral number of milliseconds')
    return rounded


def start_payload(stage, argument, seconds, payload=None):
    if stage not in range(1, 7) or not math.isfinite(argument):
        raise ValueError('Invalid stage/argument')
    if payload is None:
        if stage == 6:
            raise ValueError('Open-loop requires its explicit 16-byte payload')
        payload = struct.pack('<If', stage, argument)
    expected_lengths = (8, 12) if stage == 1 else (16,) if stage == 6 else (8,)
    if len(payload) not in expected_lengths:
        raise ValueError('Invalid Start payload length')
    wire_stage = struct.unpack_from('<I', payload)[0]
    if wire_stage != stage or payload[4:8] != struct.pack('<f', argument):
        raise ValueError('Start payload does not match stage/argument')
    if stage in (1, 2):
        minimum = 0.1 if stage == 1 else 1.0
        if argument != 0 and not minimum <= argument <= 4:
            raise ValueError('Invalid calibration current')
        if len(payload) == 12:
            voltage = struct.unpack_from('<f', payload, 8)[0]
            if not math.isfinite(voltage) or not 0.1 <= voltage <= 3:
                raise ValueError('Invalid calibration voltage')
    elif stage == 3 and argument != 0:
        raise ValueError('Zero-torque argument must be zero')
    elif stage in (4, 5) and abs(argument) > (0.3 if stage == 4 else 0.5):
        raise ValueError('Motion argument exceeds limit')
    elif stage == 6:
        _, wire_argument, current, duration = struct.unpack('<IffI', payload)
        if not 0 < abs(wire_argument) <= 20 or not 0 < abs(argument) <= 20 or not math.isfinite(current) or not 0.1 <= current <= 1.5:
            raise ValueError('Invalid open-loop speed/current')
        if duration != duration_ms(seconds):
            raise ValueError('Open-loop duration and host deadline disagree')
    return payload


def print_status(status, output=print):
    for name, value in status.items():
        if status['stage'] == 6:
            if name in ('position_turns', 'velocity_turns_s'):
                value = 'NOT_MEASURED'
            elif name in ('iq', 'id'):
                name += '_rotating_command_frame_A'
            elif name == 'argument':
                name = 'commanded_electrical_rad_s'
        output('%s: %s' % (name, value))
    if status['stage'] == 6:
        output('Open-loop currents use the commanded rotating frame, not measured rotor torque. Rotor speed and pole pairs are not measured.')


def run_phase(client, stage, argument, seconds, output=print, payload=None):
    payload = start_payload(stage, argument, seconds, payload)
    preconditions(client.bench(), stage)
    attempted = False
    try:
        # Set before sending: an acknowledgement can be lost after MCU admission.
        attempted = True
        initial = client.bench(22, payload)
        epoch = initial['epoch']
        if initial['stage'] != stage or initial['state'] not in (1, 2) or not initial['active']:
            raise ProtocolError('Start did not establish a pending/running lease')
        if stage == 6:
            requested_ms = duration_ms(seconds)
            expected_argument = struct.unpack_from('<f', payload, 4)[0]
            if initial['argument'] != expected_argument or not 0 < initial['remaining_ms'] <= requested_ms:
                raise ProtocolError('Open-loop start acknowledgement argument/deadline mismatch')
            output('Open-loop: commanded electrical speed; rotor position/speed NOT_MEASURED. iq/id are rotating command-frame currents, not rotor torque.')
        deadline = time.monotonic() + seconds + 3.0
        next_poll = time.monotonic()
        next_print = 0.0
        while True:
            now = time.monotonic()
            if now >= deadline:
                raise TimeoutError('Host phase deadline exceeded')
            if now < next_poll:
                time.sleep(next_poll - now)
            feed = None
            try:
                feed = client.bench(23)
            except CommandRejected:
                # The lease can finish between polls. Only a fresh, matching
                # terminal status can explain this; transport/CRC errors propagate.
                pass
            status = client.bench(25)
            if feed is None and status['active']:
                raise ProtocolError('Heartbeat rejected while phase is active')
            if (feed is not None and feed['epoch'] != epoch) or status['epoch'] != epoch or status['stage'] != stage:
                raise ProtocolError('Bench lease changed')
            now = time.monotonic()
            if now >= next_print or not status['active']:
                if stage == 6:
                    output('stage={stage} state={state} reason={reason} remaining_ms={remaining_ms} '
                           'armed={armed} electrical_rad_s={argument:.3f} pos=NOT_MEASURED vel=NOT_MEASURED '
                           'iq_rotating_frame_A={iq:.3f} id_rotating_frame_A={id:.3f} '
                           'vbus={vbus:.3f} hx={hx_sequence}'.format(**status))
                else:
                    output('stage={stage} state={state} reason={reason} remaining_ms={remaining_ms} '
                           'armed={armed} pos={position_turns:.4f} vel={velocity_turns_s:.4f} '
                           'iq={iq:.3f} vbus={vbus:.3f} hx={hx_sequence}'.format(**status))
                next_print = now + 1.0
            if not status['active']:
                if status['state'] != 3:
                    raise ProtocolError('Phase ended without completion: state=%d reason=%d' %
                                        (status['state'], status['reason']))
                output('Firmware phase COMPLETE; inspect logged measurements before accepting physical performance.')
                return status
            next_poll = max(next_poll + 0.1, now)
    finally:
        if attempted:
            try:
                client.bench(24)
                output('Final STOP acknowledged')
            except (Exception, KeyboardInterrupt) as error:
                output('Final STOP unconfirmed: %s; independent firmware lease timeout remains required.' %
                       ascii(str(error)))


def ascii_error(error):
    return str(error).encode('ascii', 'backslashreplace').decode('ascii')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', nargs='?', default='status',
                        choices=['status', 'clear', 'stop', *PHASES])
    parser.add_argument('--host', default='192.168.11.2')
    parser.add_argument('--port', type=int, default=39400)
    parser.add_argument('--value', type=float)
    parser.add_argument('--current', type=float,
                        help='motor-cal: 0/default4A or 0.1..4A; encoder-cal: 0/default4A or 1..4A; open-loop: 0.1..1.5A/default0.5A')
    parser.add_argument('--voltage', type=float, help='motor-cal only: explicit 0.1..3V calibration voltage')
    parser.add_argument('--seconds', type=float, help='open-loop only: 0.2..5s, integral milliseconds (default: 1s)')
    parser.add_argument('--execute', action='store_true', help='Required to send a motion phase start')
    parser.add_argument('--log-dir', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'build/rehab/bench_runs')
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error('Invalid TCP port')
    if args.command in ('velocity', 'position', 'open-loop'):
        bound = {'velocity': 0.3, 'position': 0.5, 'open-loop': 20}[args.command]
        if (args.value is None or not math.isfinite(args.value) or abs(args.value) > bound
                or (args.command == 'open-loop' and args.value == 0)):
            parser.error('--value must be finite with magnitude <= ' + str(bound))
    elif args.value is not None:
        parser.error('--value is only for velocity/position/open-loop')
    if args.command in ('motor-cal', 'encoder-cal'):
        current = 0.0 if args.current is None else args.current
        minimum = 0.1 if args.command == 'motor-cal' else 1.0
        if not math.isfinite(current) or not (current == 0.0 or minimum <= current <= 4.0):
            parser.error('--current must be 0 (default 4A) or finite %g..4A' % minimum)
        argument = current
    elif args.command == 'open-loop':
        current = 0.5 if args.current is None else args.current
        if not math.isfinite(current) or not 0.1 <= current <= 1.5:
            parser.error('--current must be finite 0.1..1.5A for open-loop')
        argument = args.value
    else:
        if args.current is not None:
            parser.error('--current is only for motor-cal/encoder-cal/open-loop')
        argument = args.value or 0.0
    payload = None
    if args.voltage is not None:
        if args.command != 'motor-cal' or not math.isfinite(args.voltage) or not 0.1 <= args.voltage <= 3.0:
            parser.error('--voltage is only for motor-cal and must be finite 0.1..3V')
        payload = struct.pack('<Iff', 1, argument, args.voltage)
    if args.command == 'open-loop':
        seconds = 1.0 if args.seconds is None else args.seconds
        try:
            duration = duration_ms(seconds)
        except ValueError as error:
            parser.error(str(error))
        payload = struct.pack('<IffI', 6, argument, current, duration)
    elif args.seconds is not None:
        parser.error('--seconds is only for open-loop')
    if payload is not None:
        try:
            stage, default_seconds = PHASES[args.command]
            start_payload(stage, argument, seconds if stage == 6 else default_seconds, payload)
        except ValueError as error:
            parser.error(str(error))
    if args.command in PHASES and not args.execute:
        print('READ-ONLY: motion was not requested; add --execute only for the intended single phase.')
        return 2
    try:
        args.log_dir.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        print('FAILED: ' + ascii_error(error), file=sys.stderr)
        return 1
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S-%fZ')
    path = args.log_dir / ('bench-' + timestamp + '.jsonl')
    try:
        with path.open('x', encoding='utf-8') as stream:
            def log(event, **data):
                record = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                              monotonic=time.monotonic(), event=event, **data)
                # Preserve nonfinite telemetry explicitly, with valid JSON syntax.
                record = {k: (str(v) if isinstance(v, float) and not math.isfinite(v) else v)
                          for k, v in record.items()}
                stream.write(json.dumps(record, ensure_ascii=True, allow_nan=False) + '\n')
                stream.flush()
            log('begin', command=args.command, host=args.host, port=args.port,
                execute=args.execute, argument=argument, requested_current=args.current,
                requested_voltage=args.voltage, requested_seconds=args.seconds)
            try:
                with socket.create_connection((args.host, args.port), timeout=2.0) as connection:
                    client = Client(connection, log)
                    client.hello()
                    print('TCP Hello: PASS')
                    if args.command in PHASES:
                        stage, phase_seconds = PHASES[args.command]
                        if stage == 6:
                            phase_seconds = seconds
                        if payload is None:
                            run_phase(client, stage, argument, phase_seconds)
                        else:
                            run_phase(client, stage, argument, phase_seconds, payload=payload)
                    else:
                        status = client.bench({'status': 25, 'clear': 26, 'stop': 24}[args.command])
                        print_status(status)
                    log('finished')
            except BaseException as error:
                log('error', error_type=type(error).__name__, message=str(error))
                raise
    except (Exception, KeyboardInterrupt) as error:
        print('FAILED: ' + ascii_error(error), file=sys.stderr)
        print('Log: ' + ascii_error(path))
        return 1
    print('Log: ' + ascii_error(path))
    return 0


if __name__ == '__main__':
    sys.exit(main())
