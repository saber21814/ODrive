#!/usr/bin/env python3
"""Offline bench client tests: mocked TCP only; never connect to hardware."""
import contextlib
import importlib.util
import io
import math
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[2] / 'Rehab/scripts/bench.py'
spec = importlib.util.spec_from_file_location('bench_client', PATH)
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


def good_status(**changes):
    status = dict.fromkeys(bench.WORDS, 0)
    status.update(dict.fromkeys(bench.FLOATS, 0.0))
    status.update(version=1, enabled=1, axis_state=1, gate_ready=1, encoder_cpr=4000,
                  adc_packed=0x87000800, vbus=24.0, fet_temperature_c=25.0,
                  aux_temperature_c=25.0, motor_passed=1, motor_calibrated=1, encoder_passed=1,
                  encoder_count_bits=0xffffffff, epoch=7)
    status.update(changes)
    return status


def status_payload(status=None):
    status = good_status() if status is None else status
    return struct.pack('<36I12f', 0, *[status[n] for n in bench.WORDS],
                       *[status[n] for n in bench.FLOATS])


class Socket:
    def __init__(self, modify=lambda x: x, chunk=1):
        self.buffer = bytearray()
        self.requests = []
        self.modify = modify
        self.chunk = chunk
        self.timeouts = []
        self.closed = False

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.closed = True

    def settimeout(self, timeout):
        self.timeouts.append(timeout)

    def sendall(self, raw):
        magic, version, flags, n, command, session, sequence = bench.HEADER.unpack(raw[:16])
        assert magic == b'RH' and version == 1 and flags == 0 and len(raw) == 20 + n
        self.requests.append((command, session, sequence, raw[16:-4]))
        payload = struct.pack('<4I', 0, 1, 256, 0x1201) if command == 1 else status_payload()
        reply = bench.frame(command, 123 if command == 1 else session, sequence, payload, 1)
        self.buffer.extend(self.modify(reply))

    def recv(self, n):
        count = min(n, self.chunk, len(self.buffer))
        data = bytes(self.buffer[:count])
        del self.buffer[:count]
        return data


class Clock:
    def __init__(self):
        self.now = 10.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class PhaseClient:
    def __init__(self, failure=None, terminal=3, reject_feed=False):
        self.calls = []
        self.failure = failure
        self.terminal = terminal
        self.reject_feed = reject_feed
        self.started = False
        self.stage = 4
        self.polls = 0
        self.argument = 0.0
        self.remaining = 0

    def bench(self, command=25, payload=b''):
        self.calls.append((command, payload))
        if command == self.failure:
            raise bench.ProtocolError('injected failure')
        if command == 22:
            self.stage, self.argument = struct.unpack_from('<If', payload)
            self.remaining = struct.unpack_from('<I', payload, 12)[0] if self.stage == 6 else 1000
            self.started = True
            return good_status(active=1, stage=self.stage, state=1, argument=self.argument, remaining_ms=self.remaining)
        if command == 23:
            if self.reject_feed:
                raise bench.CommandRejected(23, 5)
            return good_status(active=1, stage=self.stage, state=2, argument=self.argument, remaining_ms=self.remaining)
        if command == 25 and self.started:
            self.polls += 1
            return good_status(stage=self.stage, state=self.terminal,
                               active=int(self.terminal in (1, 2)), argument=self.argument, remaining_ms=self.remaining)
        return good_status()


class ClientTests(unittest.TestCase):
    def test_fragmented_hello_status(self):
        connection = Socket(chunk=1)
        client = bench.Client(connection)
        client.hello()
        status = client.bench()
        self.assertEqual(status['encoder_count'], -1)
        self.assertEqual(status['vbus'], 24)
        self.assertEqual([x[0] for x in connection.requests], [1, 25])
        self.assertEqual([x[2] for x in connection.requests], [1, 2])
        self.assertTrue(all(0 < x <= 2 for x in connection.timeouts))
        with self.assertRaises(bench.ProtocolError):
            client.hello()

    def test_crc_and_response_identity(self):
        for mode in ('crc', 'sequence', 'command', 'session', 'flags', 'length', 'version'):
            def modify(raw):
                if mode == 'crc':
                    return raw[:-1] + bytes([raw[-1] ^ 1])
                magic, version, flags, length, cmd, session, sequence = bench.HEADER.unpack(raw[:16])
                if mode == 'sequence': sequence += 1
                if mode == 'command': cmd += 1
                if mode == 'session': session += 1
                if mode == 'flags': flags = 0
                if mode == 'version': version = 2
                if mode == 'length': length = 257
                header = bench.HEADER.pack(magic, version, flags, length, cmd, session, sequence)
                changed = header + raw[16:-4]
                return changed + struct.pack('<I', bench.zlib.crc32(changed))
            with self.subTest(mode=mode):
                client = bench.Client(Socket(modify=modify))
                client.session = 123
                with self.assertRaises(bench.ProtocolError):
                    client.bench()

    def test_short_error_response(self):
        def rejected(raw):
            _, _, _, _, cmd, session, sequence = bench.HEADER.unpack(raw[:16])
            return bench.frame(cmd, session, sequence, struct.pack('<I', 6), 1)
        connection = Socket(modify=rejected)
        client = bench.Client(connection)
        client.session = 123
        with self.assertRaises(bench.CommandRejected) as error:
            client.bench()
        self.assertEqual(error.exception.command, 25)
        self.assertEqual(error.exception.result, 6)
        self.assertEqual(len(connection.requests), 1)

    def test_eof_and_shared_deadline(self):
        connection = Socket(modify=lambda _: b'')
        with self.assertRaises(ConnectionError):
            bench.Client(connection).hello()
        clock = Clock()
        connection = Socket(chunk=1)
        original = connection.recv
        def slow_read(n):
            clock.now += 0.2
            return original(n)
        connection.recv = slow_read
        with patch.object(bench.time, 'monotonic', clock.monotonic):
            with self.assertRaises(TimeoutError):
                bench.Client(connection, timeout=0.5).hello()
        self.assertLess(len(connection.timeouts), 10)

    def test_status_shape(self):
        self.assertEqual(len(bench.WORDS), 35)
        self.assertEqual(len(bench.FLOATS), 12)
        self.assertEqual(len(status_payload()), 192)
        for payload in (b'', status_payload()[:-4], status_payload(good_status(version=2))):
            with self.assertRaises(bench.ProtocolError):
                bench.parse_status(payload)

    def test_preconditions(self):
        bench.preconditions(good_status(), 4)
        for changes in ({'enabled': 0}, {'active': 1}, {'armed': 1}, {'axis_state': 8},
                        {'system_error': 1}, {'motor_error_hi': 1}, {'physical_stop': 1},
                        {'gate_ready': 0}, {'encoder_cpr': 8192}, {'adc_age_ms': 11},
                        {'adc_packed': 0x8f000000}, {'adc_packed': 0x80000000},
                        {'vbus': 27}, {'fet_temperature_c': math.nan},
                        {'aux_temperature_c': 51}, {'velocity_turns_s': math.inf},
                        {'motor_passed': 0}, {'encoder_passed': 0}, {'state': 5}):
            with self.subTest(changes=changes):
                with self.assertRaises(bench.ProtocolError):
                    bench.preconditions(good_status(**changes), 4)
        bench.preconditions(good_status(motor_passed=0, encoder_passed=0), 1)
        bench.preconditions(good_status(encoder_passed=0), 2)

    def test_precondition_temperature_and_stationary_boundaries(self):
        for temperature in ('fet_temperature_c', 'aux_temperature_c'):
            for value in (-10.0, 49.99):
                bench.preconditions(good_status(**{temperature: value}), 4)
            for value in (-10.01, 50.0):
                with self.subTest(field=temperature, value=value), self.assertRaises(bench.ProtocolError):
                    bench.preconditions(good_status(**{temperature: value}), 4)
        for value in (-0.02, 0.02):
            bench.preconditions(good_status(velocity_turns_s=value), 4)
        for value in (-0.0201, 0.0201):
            with self.subTest(velocity=value), self.assertRaises(bench.ProtocolError):
                bench.preconditions(good_status(velocity_turns_s=value), 4)
        for value in (22.0, 26.0):
            bench.preconditions(good_status(vbus=value), 4)

    def test_single_start_and_final_stop(self):
        client = PhaseClient()
        result = bench.run_phase(client, 4, 0.1, 10, output=lambda *_: None)
        self.assertEqual(result['state'], 3)
        self.assertEqual([c for c, _ in client.calls], [25, 22, 23, 25, 24])

    def test_terminal_feed_rejection_only(self):
        client = PhaseClient(reject_feed=True)
        bench.run_phase(client, 4, 0.1, 10, output=lambda *_: None)
        client = PhaseClient(reject_feed=True, terminal=2)
        with self.assertRaises(bench.ProtocolError):
            bench.run_phase(client, 4, 0.1, 10, output=lambda *_: None)
        self.assertEqual(client.calls[-1][0], 24)
        client = PhaseClient(failure=23)
        with self.assertRaises(bench.ProtocolError):
            bench.run_phase(client, 4, 0.1, 10, output=lambda *_: None)
        # Transport/CRC errors do not get ignored or retried as status.
        self.assertEqual([c for c, _ in client.calls], [25, 22, 23, 24])

    def test_start_exception_and_ctrl_c_stop(self):
        client = PhaseClient(failure=22)
        with self.assertRaises(bench.ProtocolError):
            bench.run_phase(client, 4, 0.1, 10, output=lambda *_: None)
        self.assertEqual([c for c, _ in client.calls], [25, 22, 24])
        client = PhaseClient()
        original = client.bench
        def interrupted(command=25, payload=b''):
            if command == 23:
                raise KeyboardInterrupt()
            return original(command, payload)
        client.bench = interrupted
        with self.assertRaises(KeyboardInterrupt):
            bench.run_phase(client, 4, 0.1, 10, output=lambda *_: None)
        self.assertEqual(client.calls[-1][0], 24)

    def test_host_deadline(self):
        clock = Clock()
        client = PhaseClient(terminal=2)
        with patch.object(bench.time, 'monotonic', clock.monotonic), patch.object(bench.time, 'sleep', clock.sleep):
            with self.assertRaises(TimeoutError):
                bench.run_phase(client, 4, 0.1, 0, output=lambda *_: None)
        self.assertEqual(sum(c == 22 for c, _ in client.calls), 1)
        self.assertEqual(client.calls[-1][0], 24)
        self.assertLess(len(client.calls), 75)

    def test_no_execute_never_connects(self):
        with patch.object(bench.socket, 'create_connection') as connect, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(bench.main(['motor-cal']), 2)
            self.assertEqual(bench.main(['velocity', '--value', '0.1']), 2)
            connect.assert_not_called()

    def test_calibration_current_boundaries(self):
        with patch.object(bench.socket, 'create_connection') as connect, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            for phase in ('motor-cal', 'encoder-cal'):
                for value in ('0', '1', '2.5', '4'):
                    self.assertEqual(bench.main([phase, '--current', value]), 2)
                invalid = ('-1', '0.099', '4.01', 'nan', 'inf', '-inf')
                if phase == 'encoder-cal':
                    invalid += ('0.99',)
                for value in invalid:
                    with self.subTest(phase=phase, value=value), self.assertRaises(SystemExit) as error:
                        bench.main([phase, '--current=' + value, '--execute'])
                    self.assertEqual(error.exception.code, 2)
            for phase in ('status', 'clear', 'stop', 'zero'):
                with self.assertRaises(SystemExit):
                    bench.main([phase, '--current', '0'])
            connect.assert_not_called()

    def test_calibration_current_routes_start_argument(self):
        root = PATH.parents[2] / 'build/rehab/bench_client_tests'
        root.mkdir(parents=True, exist_ok=True)
        for command, stage in (('motor-cal', 1), ('encoder-cal', 2)):
            for value in (None, 0.0, 1.0, 4.0):
                with tempfile.TemporaryDirectory(dir=root) as directory:
                    args = [command, '--execute', '--log-dir', directory]
                    if value is not None:
                        args += ['--current', str(value)]
                    with patch.object(bench.socket, 'create_connection', return_value=Socket()), patch.object(bench, 'run_phase') as run, contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(bench.main(args), 0)
                    self.assertEqual(run.call_count, 1)
                    self.assertEqual(run.call_args.args[1:3], (stage, value or 0.0))

    def test_sensorless_preconditions_only_for_motor_cal_and_open_loop(self):
        missing = good_status(encoder_cpr=0, position_turns=math.nan,
                              velocity_turns_s=math.nan, encoder_passed=0)
        bench.preconditions(missing, 1)
        bench.preconditions(missing, 6)
        for stage in (2, 3, 4, 5):
            with self.assertRaises(bench.ProtocolError):
                bench.preconditions(missing, stage)
        for key in ('motor_passed', 'motor_calibrated'):
            with self.assertRaises(bench.ProtocolError):
                bench.preconditions(good_status(**{key: 0}), 6)
        for stage in (1, 6):
            for key in ('system_error', 'axis_error', 'motor_error_lo', 'encoder_error', 'controller_error'):
                with self.assertRaises(bench.ProtocolError):
                    bench.preconditions(good_status(**{key: 1}), stage)

    def test_extended_arguments_validate_before_connect(self):
        valid = [
            ['motor-cal', '--current', '0.1', '--voltage', '0.1'],
            ['motor-cal', '--current', '0.5', '--voltage', '0.5'],
            ['motor-cal', '--current', '4', '--voltage', '3'],
            ['open-loop', '--value', '-20', '--current', '0.1', '--seconds', '0.2'],
            ['open-loop', '--value', '20', '--current', '1.5', '--seconds', '5'],
            ['open-loop', '--value', '1'],
            ['open-loop', '--value', '-1', '--seconds', '1.001'],
        ]
        invalid = [
            ['open-loop'], ['open-loop', '--value', '0'],
            ['encoder-cal', '--voltage', '0.5'], ['zero', '--seconds', '1'],
            ['status', '--seconds', '1'], ['velocity', '--value', '0.1', '--voltage', '0.5'],
        ]
        for value in ('nan', 'inf', '-inf', '20.01', '-20.01', '1e-50', '-1e-50'):
            invalid.append(['open-loop', '--value=' + value])
        for value in ('0', '0.099', '1.501', 'nan', 'inf'):
            invalid.append(['open-loop', '--value', '1', '--current=' + value])
        for value in ('0', '0.199', '5.001', '1.0001', 'nan', 'inf'):
            invalid.append(['open-loop', '--value', '1', '--seconds=' + value])
        for value in ('0', '0.099', '3.001', 'nan', 'inf'):
            invalid.append(['motor-cal', '--voltage=' + value])
        with patch.object(bench.socket, 'create_connection') as connect, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            for args in valid:
                self.assertEqual(bench.main(args), 2)
            for args in invalid:
                with self.subTest(args=args), self.assertRaises(SystemExit):
                    bench.main(args + ['--execute'])
            connect.assert_not_called()

    def test_extended_wire_payload_routes(self):
        root = PATH.parents[2] / 'build/rehab/bench_client_tests'
        root.mkdir(parents=True, exist_ok=True)
        cases = [
            (['motor-cal', '--current', '0.5', '--voltage', '0.5'], 1, 0.5, 8, struct.pack('<Iff', 1, 0.5, 0.5)),
            (['open-loop', '--value', '-2'], 6, -2.0, 1.0, struct.pack('<IffI', 6, -2.0, 0.5, 1000)),
            (['open-loop', '--value', '20', '--current', '1.5', '--seconds', '0.2'], 6, 20.0, 0.2, struct.pack('<IffI', 6, 20.0, 1.5, 200)),
        ]
        for args, stage, argument, seconds, payload in cases:
            with tempfile.TemporaryDirectory(dir=root) as directory, patch.object(bench.socket, 'create_connection', return_value=Socket()), patch.object(bench, 'run_phase') as run, contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(bench.main(args + ['--execute', '--log-dir', directory]), 0)
                self.assertEqual(run.call_count, 1)
                self.assertEqual(run.call_args.args[1:], (stage, argument, seconds))
                self.assertEqual(run.call_args.kwargs, {'payload': payload})

    def test_start_payload_validation_is_before_any_request(self):
        for payload in (b'', struct.pack('<If', 6, 1), struct.pack('<Iff', 6, 1, 0.5),
                        struct.pack('<IffI', 5, 1, 0.5, 1000), struct.pack('<IffI', 6, 2, 0.5, 1000),
                        struct.pack('<IffI', 6, 1, math.nan, 1000), struct.pack('<IffI', 6, 1, 0.5, 999),
                        struct.pack('<IffI', 6, 1, 0.5, 1000) + b'1234'):
            client = PhaseClient()
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                bench.run_phase(client, 6, 1, 1, payload=payload)
            self.assertEqual(client.calls, [])
        for voltage in (math.nan, 0.0, 3.1):
            client = PhaseClient()
            with self.assertRaises(ValueError):
                bench.run_phase(client, 1, 0.5, 8, payload=struct.pack('<Iff', 1, 0.5, voltage))
            self.assertEqual(client.calls, [])
        old = struct.pack('<If', 1, 0.0)
        self.assertEqual(bench.start_payload(1, 0, 8), old)
        self.assertEqual(bench.start_payload(2, 0, 15), struct.pack('<If', 2, 0))

    def test_open_loop_ack_output_and_final_stop(self):
        client = PhaseClient()
        output = []
        payload = struct.pack('<IffI', 6, -2, 0.5, 1000)
        result = bench.run_phase(client, 6, -2, 1, output=output.append, payload=payload)
        self.assertEqual(result['state'], 3)
        self.assertEqual([c for c, _ in client.calls], [25, 22, 23, 25, 24])
        self.assertEqual(client.calls[1][1], payload)
        text = '\n'.join(output)
        self.assertIn('pos=NOT_MEASURED vel=NOT_MEASURED', text)
        self.assertIn('iq_rotating_frame_A=', text)
        self.assertIn('not rotor torque', text)
        labels = []
        bench.print_status(good_status(stage=6, position_turns=123, velocity_turns_s=45), labels.append)
        self.assertIn('position_turns: NOT_MEASURED', labels)
        self.assertIn('velocity_turns_s: NOT_MEASURED', labels)
        self.assertNotIn('position_turns: 123', labels)

    def test_open_loop_ack_mismatch_stops_without_feed(self):
        for changes in ({'argument': 2.0}, {'remaining_ms': 1001}, {'remaining_ms': 0}, {'epoch': 8}):
            client = PhaseClient()
            original = client.bench
            def changed(command=25, payload=b''):
                status = original(command, payload)
                if command == (25 if 'epoch' in changes and client.started else 22):
                    status.update(changes)
                return status
            client.bench = changed
            with self.subTest(changes=changes), self.assertRaises(bench.ProtocolError):
                bench.run_phase(client, 6, 1, 1, output=lambda *_: None, payload=struct.pack('<IffI', 6, 1, 0.5, 1000))
            self.assertEqual(sum(c == 22 for c, _ in client.calls), 1)
            self.assertEqual(client.calls[-1][0], 24)

    def test_open_loop_host_timeout_and_old_firmware_no_retry(self):
        clock = Clock()
        client = PhaseClient(terminal=2)
        payload = struct.pack('<IffI', 6, 1, 0.5, 200)
        with patch.object(bench.time, 'monotonic', clock.monotonic), patch.object(bench.time, 'sleep', clock.sleep):
            with self.assertRaises(TimeoutError):
                bench.run_phase(client, 6, 1, 0.2, output=lambda *_: None, payload=payload)
        self.assertEqual(sum(c == 22 for c, _ in client.calls), 1)
        self.assertEqual(client.calls[-1][0], 24)
        client = PhaseClient()
        original = client.bench
        def old_firmware(command=25, payload=b''):
            if command == 22:
                client.calls.append((command, payload))
                raise bench.CommandRejected(22, 2)
            return original(command, payload)
        client.bench = old_firmware
        with self.assertRaises(bench.CommandRejected):
            bench.run_phase(client, 6, 1, 0.2, output=lambda *_: None, payload=payload)
        self.assertEqual([c for c, _ in client.calls], [25, 22, 24])

    def test_default_is_status_and_jsonl(self):
        connection = Socket()
        root = PATH.parents[2] / 'build/rehab/bench_client_tests'
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as directory:
            with patch.object(bench.socket, 'create_connection', return_value=connection), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(bench.main(['--log-dir', directory]), 0)
            self.assertEqual([x[0] for x in connection.requests], [1, 25])
            logs = list(Path(directory).glob('*.jsonl'))
            self.assertEqual(len(logs), 1)
            records = [bench.json.loads(line) for line in logs[0].read_text().splitlines()]
            self.assertTrue(any(x['event'] == 'tx' and 'raw_hex' in x for x in records))
            self.assertTrue(any(x['event'] == 'rx' and 'raw_hex' in x for x in records))
            self.assertTrue(any(x['event'] == 'status' and x['version'] == 1 for x in records))
            self.assertEqual(records[-1]['event'], 'finished')
            self.assertTrue(connection.closed)


if __name__ == '__main__':
    unittest.main()
