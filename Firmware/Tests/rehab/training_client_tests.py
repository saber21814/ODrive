"""Pure-host tests: localhost fake TCP only; never connect to a controller.

Run: python Firmware/Tests/rehab/training_client_tests.py
"""
import importlib.util
from pathlib import Path
import socket
import struct
import threading
import time
import unittest
import zlib


CLIENT_PATH = Path(__file__).resolve().parents[2] / 'Rehab/scripts/training_client.py'
spec = importlib.util.spec_from_file_location('training_client', CLIENT_PATH)
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)
H = struct.Struct('<2sBBHHII')
C = client.Command


def packet(command, session, sequence, payload=b'', flags=1, version=1):
    raw = H.pack(b'RH', version, flags, len(payload), command, session, sequence) + payload
    return raw + struct.pack('<I', zlib.crc32(raw))


def receive(sock, n):
    data = b''
    while len(data) < n:
        part = sock.recv(n - len(data))
        if not part:
            raise EOFError()
        data += part
    return data


class FakeServer:
    def __init__(self, hook=None, fragment=False):
        self.hook, self.fragment = hook, fragment
        self.requests, self.errors = [], []
        self.changed = threading.Condition()
        self.done = threading.Event()
        self.sock = socket.socket()
        self.sock.bind(('127.0.0.1', 0))
        self.sock.listen()
        self.sock.settimeout(0.05)
        self.port = self.sock.getsockname()[1]
        self.peer = None
        self.connection_count = 0
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.done.is_set():
            try:
                peer, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.peer = peer
            self.connection_count += 1
            session = 1000 + self.connection_count
            try:
                while not self.done.is_set():
                    head = receive(peer, 16)
                    magic, version, flags, length, cmd, sess, seq = H.unpack(head)
                    data = receive(peer, length + 4)
                    assert (magic, version, flags) == (b'RH', 1, 0)
                    assert length <= 256
                    assert zlib.crc32(head + data[:-4]) == struct.unpack('<I', data[-4:])[0]
                    assert sess == (0 if cmd == C.HELLO else session)
                    request = (cmd, sess, seq, data[:-4])
                    with self.changed:
                        self.requests.append(request)
                        self.changed.notify_all()
                    body = self.body(cmd, data[:-4])
                    response = packet(cmd, session, seq, struct.pack('<I', 0) + body)
                    if self.hook:
                        response = self.hook(request, response)
                    if response is None:
                        continue
                    if self.fragment:
                        for byte in response:
                            peer.sendall(bytes((byte,)))
                    else:
                        peer.sendall(response)
            except (EOFError, OSError):
                pass
            except Exception as exc:
                self.errors.append(exc)
            finally:
                peer.close()
                self.peer = None

    def body(self, cmd, payload):
        if cmd == C.HELLO:
            return struct.pack('<III', 1, 256, 0x1201)
        if cmd == C.STATUS:
            return struct.pack('<8I4f3I', 0, 0, 0, 0, 1, 1, 1, 100,
                               0.5, 0.1, 12.5, 0.2, 0, 1, 1)
        if cmd == C.DIAGNOSTICS:
            return struct.pack('<63I', *range(63))
        if cmd == C.STATISTICS:
            return struct.pack('<2I10fI', 2, 3, *range(10), 1)
        if cmd == C.GET_LOG:
            return struct.pack('<4I', 1, 20, 30, 40)
        if cmd == C.GET_CONFIG:
            first, count, staged = struct.unpack('<HHI', payload)
            assert staged in (0, 1)
            return b''.join(struct.pack('<HH', i, int(i in client.INTEGER_FIELDS)) +
                            struct.pack('<I' if i in client.INTEGER_FIELDS else '<f', i)
                            for i in range(first, first + count))
        return b''

    def wait_for(self, cmd, count=1, timeout=1):
        with self.changed:
            return self.changed.wait_for(
                lambda: sum(r[0] == cmd for r in self.requests) >= count, timeout)

    def close(self):
        self.done.set()
        if self.peer:
            try:
                self.peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.sock.close()
        self.thread.join(1)


class ClientTests(unittest.TestCase):
    def server(self, *args, **kwargs):
        server = FakeServer(*args, **kwargs)
        self.addCleanup(server.close)
        return server

    def connect(self, server, **kwargs):
        c = client.TrainingClient('127.0.0.1', server.port, **kwargs).connect()
        self.addCleanup(c.close)
        return c

    def test_connect_observes_only_and_close_does_not_release(self):
        server = self.server()
        c = self.connect(server)
        self.assertTrue(c.connected)
        self.assertFalse(c.acquired)
        self.assertEqual(c.session, 1001)
        self.assertFalse(server.wait_for(C.HEARTBEAT, timeout=0.08))
        self.assertEqual([r[0] for r in server.requests], [C.HELLO])
        c.close()
        self.assertEqual([r[0] for r in server.requests], [C.HELLO])
        self.assertEqual(c.session, 0)

    def test_reads_and_fragmented_frames(self):
        server = self.server(fragment=True)
        c = self.connect(server, timeout=1)
        self.assertEqual(c.status()['force_n'], 12.5)
        self.assertEqual(c.diagnostics(), tuple(range(63)))
        self.assertEqual(c.statistics()['repetitions'], 2)
        self.assertEqual(c.get_log()['code'], 30)
        self.assertFalse(server.errors)

    def test_malformed_frames_fail_closed(self):
        def alter(response, offset, value):
            raw = bytearray(response[:-4])
            raw[offset] = value
            return bytes(raw) + struct.pack('<I', zlib.crc32(raw))

        mutations = {
            'magic': lambda p: alter(p, 0, 0),
            'version': lambda p: alter(p, 2, 2),
            'flags': lambda p: alter(p, 3, 0),
            'too_long': lambda p: alter(p, 5, 2),
            'too_short': lambda p: alter(p, 4, 0),
            'command': lambda p: alter(p, 6, 20),
            'session': lambda p: alter(p, 8, 0),
            'sequence': lambda p: alter(p, 12, 99),
            'crc': lambda p: p[:-1] + bytes((p[-1] ^ 1,)),
            'body_length': lambda p: packet(C.STATUS, 1001, 2, struct.pack('<I', 0)),
            'result': lambda p: packet(C.STATUS, 1001, 2, struct.pack('<I', 999)),
            'error_length': lambda p: packet(C.STATUS, 1001, 2, struct.pack('<II', 3, 0)),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                server = self.server(lambda r, p: mutate(p) if r[0] == C.STATUS else p)
                c = self.connect(server)
                with self.assertRaises(client.ProtocolError):
                    c.status()
                self.assertFalse(c.connected)
                self.assertIsInstance(c.last_error, client.ProtocolError)
                with self.assertRaises(client.ProtocolError):
                    c.check_error()
                self.assertEqual(len(server.requests), 2)

    def test_invalid_hello(self):
        for session, body in ((0, (1, 256, 0x1201)), (9, (2, 256, 0x1201)),
                              (9, (1, 128, 0x1201)), (9, (1, 256, 0))):
            with self.subTest(session=session, body=body):
                server = self.server(lambda r, p: packet(C.HELLO, session, r[2],
                                                        struct.pack('<4I', 0, *body)))
                c = client.TrainingClient('127.0.0.1', server.port)
                with self.assertRaises(client.ProtocolError):
                    c.connect()
                self.assertFalse(c.connected)

    def test_timeout_no_retry_and_explicit_reconnect(self):
        server = self.server(lambda r, p: None if r[0] == C.STATUS else p)
        c = self.connect(server, timeout=0.05)
        with self.assertRaises(TimeoutError):
            c.status()
        self.assertFalse(c.connected)
        self.assertEqual([r[0] for r in server.requests], [C.HELLO, C.STATUS])
        c.connect()
        self.assertEqual(c.session, 1002)
        self.assertIsNone(c.last_error)
        self.assertEqual(server.requests[-1], (C.HELLO, 0, 1, b''))
        self.assertFalse(server.wait_for(C.HEARTBEAT, timeout=0.08))

    def test_peer_disconnect_and_reconnect_never_reacquires(self):
        disconnected = threading.Event()

        def hook(request, response):
            if request[0] == C.HEARTBEAT:
                server.peer.shutdown(socket.SHUT_RDWR)
                disconnected.set()
                return None
            return response

        server = self.server(hook)
        c = self.connect(server, heartbeat_interval=0.01)
        c.acquire()
        self.assertTrue(disconnected.wait(1))
        deadline = time.monotonic() + 1
        while c.connected and time.monotonic() < deadline:
            threading.Event().wait(0.005)
        self.assertFalse(c.connected)
        self.assertIsInstance(c.last_error, ConnectionError)
        c.connect()
        self.assertEqual(c.session, 1002)
        self.assertFalse(c.acquired)
        self.assertFalse(server.wait_for(C.HEARTBEAT, 2, 0.06))
        self.assertEqual(sum(r[0] == C.ACQUIRE for r in server.requests), 1)

    def test_sequence_exhaustion_requires_explicit_reconnect(self):
        server = self.server()
        c = self.connect(server)
        c._sequence = 0xffffffff
        with self.assertRaises(client.ProtocolError):
            c.status()
        self.assertFalse(c.connected)
        self.assertEqual(len(server.requests), 1)

    def test_heartbeat_acquire_finish_release(self):
        server = self.server()
        c = self.connect(server, heartbeat_interval=0.01)
        c.acquire()
        self.assertTrue(server.wait_for(C.HEARTBEAT, 2))
        c.finish()
        before = sum(r[0] == C.HEARTBEAT for r in server.requests)
        self.assertTrue(server.wait_for(C.HEARTBEAT, before + 1))
        c.release()
        before = sum(r[0] == C.HEARTBEAT for r in server.requests)
        self.assertFalse(server.wait_for(C.HEARTBEAT, before + 1, 0.06))
        self.assertTrue(c.connected)
        self.assertFalse(c.acquired)
        c.close()
        self.assertEqual(sum(r[0] == C.RELEASE for r in server.requests), 1)

    def test_background_failure_observable_and_never_retried(self):
        server = self.server(lambda r, p: packet(r[0], r[1], r[2], struct.pack('<I', 4))
                             if r[0] == C.HEARTBEAT else p)
        c = self.connect(server, heartbeat_interval=0.01)
        c.acquire()
        self.assertTrue(server.wait_for(C.HEARTBEAT))
        deadline = time.monotonic() + 1
        while c.connected and time.monotonic() < deadline:
            threading.Event().wait(0.005)
        self.assertFalse(c.connected)
        self.assertFalse(c.acquired)
        with self.assertRaises(client.CommandError) as error:
            c.check_error()
        self.assertEqual(error.exception.result, 4)
        self.assertFalse(server.wait_for(C.HEARTBEAT, 2, 0.06))

    def test_acquire_failure_never_starts_heartbeat(self):
        server = self.server(lambda r, p: packet(r[0], r[1], r[2], struct.pack('<I', 3))
                             if r[0] == C.ACQUIRE else p)
        c = self.connect(server)
        with self.assertRaises(client.CommandError):
            c.acquire()
        self.assertFalse(server.wait_for(C.HEARTBEAT, timeout=0.08))

    def test_owned_context_exit_disconnects_without_release(self):
        server = self.server()
        with client.TrainingClient('127.0.0.1', server.port) as c:
            c.acquire()
        self.assertFalse(c.connected)
        self.assertEqual([r[0] for r in server.requests], [C.HELLO, C.ACQUIRE])

    def test_parallel_requests_and_heartbeats_are_serialized(self):
        server = self.server()
        c = self.connect(server, heartbeat_interval=0.002)
        c.acquire()
        errors = []

        def read():
            try:
                for _ in range(15):
                    self.assertEqual(c.status()['force_n'], 12.5)
            except Exception as exc:
                errors.append(exc)

        workers = [threading.Thread(target=read) for _ in range(6)]
        for thread in workers: thread.start()
        for thread in workers: thread.join(3)
        self.assertTrue(all(not t.is_alive() for t in workers))
        self.assertTrue(server.wait_for(C.HEARTBEAT))
        c.release()
        self.assertFalse(errors)
        self.assertFalse(server.errors)
        self.assertEqual([r[2] for r in server.requests], list(range(1, len(server.requests) + 1)))

    def test_config_wire_and_explicit_commit(self):
        server = self.server()
        c = self.connect(server)
        c.set_config(radius_m=0.25, heartbeat_timeout_ms=250, flags=3)
        self.assertEqual(server.requests[-1][3],
                         struct.pack('<HHfHHIHHI', 0, 0, 0.25, 26, 1, 250, 38, 1, 3))
        self.assertNotIn(C.COMMIT_CONFIG, [r[0] for r in server.requests])
        c.commit_config()
        self.assertEqual(server.requests[-1][0], C.COMMIT_CONFIG)
        values = c.get_config(staged=True)
        self.assertEqual(len(values), 39)
        self.assertEqual(values['radius_m'], 0.0)
        self.assertEqual(values['flags'], 38)
        self.assertIs(type(values['phase_dwell_ms']), int)
        self.assertEqual([r[3] for r in server.requests if r[0] == C.GET_CONFIG],
                         [struct.pack('<HHI', 0, 31, 1), struct.pack('<HHI', 31, 8, 1)])

    def test_bad_config_type_fails_closed(self):
        server = self.server(lambda r, p: packet(r[0], r[1], r[2],
                             struct.pack('<IHHf', 0, 0, 1, 0.25)) if r[0] == C.GET_CONFIG else p)
        c = self.connect(server)
        with self.assertRaises(client.ProtocolError):
            c.get_config(count=1)
        self.assertFalse(c.connected)

    def test_explicit_commands_payloads_and_no_retired_ids(self):
        server = self.server()
        c = self.connect(server, heartbeat_interval=0.2)
        c.acquire()
        c.enable()
        c.start('isotonic')
        c.pause()
        c.resume()
        c.finish()
        c.stop()
        c.clear_faults()
        c.tare()
        c.calibrate(100.0)
        c.release()
        c.set_network('192.168.11.2', '192.168.11.1', '255.255.255.0')
        by_cmd = {r[0]: r[3] for r in server.requests}
        self.assertEqual(by_cmd[C.START], struct.pack('<I', 0))
        self.assertEqual(by_cmd[C.CALIBRATE], struct.pack('<f', 100.0))
        self.assertEqual(by_cmd[C.SET_NETWORK], bytes((192, 168, 11, 2, 192, 168, 11, 1,
                                                     255, 255, 255, 0)) + struct.pack('<H', 39400))
        self.assertTrue(all(1 <= r[0] <= 21 for r in server.requests))

    def test_local_input_validation_sends_nothing(self):
        server = self.server()
        c = self.connect(server)
        for bad in (lambda: c.start(7), lambda: c.set_config(flags=-1),
                    lambda: c.set_config(flags=1.5), lambda: c.set_config(radius_m=float('nan')),
                    lambda: c.get_config(count=40), lambda: c.calibrate(float('inf')),
                    *(lambda retired=retired: c._call(retired) for retired in range(22, 27))):
            with self.assertRaises(ValueError): bad()
        self.assertEqual(len(server.requests), 1)
        self.assertTrue(c.connected)
        for timeout in (0, -1, 2.1, float('nan')):
            with self.assertRaises(ValueError): client.TrainingClient(timeout=timeout)


if __name__ == '__main__':
    unittest.main()
