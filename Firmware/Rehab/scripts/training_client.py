"""Explicit TCP client for the existing Rehab v1 service (stdlib only).

In odrivetool: ns = runpy.run_path('.../training_client.py')
c = ns['TrainingClient']().connect(); c.status(); c.diagnostics()
Connecting sends only Hello. Acquire, configure, enable and start separately.
finish() requests a normal finish; release() relinquishes the lease explicitly.
close()/context exit only disconnect: firmware stops and retains an owned lease.
After any error inspect last_error and reconnect explicitly; nothing is retried.
"""

import ipaddress
import math
import socket
import struct
import threading
import time
import zlib
from enum import IntEnum


class Command(IntEnum):
    HELLO = 1
    STATUS = 2
    SET_CONFIG = 3
    GET_CONFIG = 4
    COMMIT_CONFIG = 5
    ACQUIRE = 6
    RELEASE = 7
    ENABLE = 8
    START = 9
    PAUSE = 10
    RESUME = 11
    STOP = 12
    CLEAR_FAULTS = 13
    HEARTBEAT = 14
    TARE = 15
    CALIBRATE = 16
    STATISTICS = 17
    DIAGNOSTICS = 18
    SET_NETWORK = 19
    GET_LOG = 20
    FINISH = 21


class Mode(IntEnum):
    ISOTONIC = 0
    CONCENTRIC = 1
    ECCENTRIC = 2
    ISOKINETIC = 3
    ELASTICITY = 4
    WATER_RESISTANCE = 5
    FITNESS = 6


RESULTS = ('Ok', 'InvalidState', 'InvalidConfig', 'Busy', 'NotOwner',
           'Faulted', 'Unsupported', 'Stale', 'BadSequence', 'QueueFull', 'Inhibited')
CONFIG_FIELDS = (
    'radius_m', 'motor_turns_per_drum_turn', 'encoder_turns_per_motor_turn',
    'direction', 'zero_turns', 'efficiency', 'force_scale_n_per_count',
    'force_offset_counts', 'force_filter_hz', 'min_length_m', 'max_length_m',
    'max_speed_m_s', 'max_force_n', 'max_torque_nm', 'torque_slew_nm_s',
    'force_kp', 'force_ki', 'integrator_limit_nm', 'concentric_force_n',
    'eccentric_force_n', 'speed_limit_m_s', 'elastic_n_m', 'water_n_s2_m2',
    'phase_speed_hysteresis_m_s', 'phase_min_distance_m', 'phase_dwell_ms',
    'heartbeat_timeout_ms', 'sensor_timeout_ms', 'target_timeout_ms',
    'stop_timeout_ms', 'return_speed_m_s', 'return_timeout_ms',
    'fitness_duration_ms', 'rfd_window_ms', 'friction_nm', 'viscous_nm_s',
    'inertia_kg_m2', 'compensation_limit_nm', 'flags')
INTEGER_FIELDS = frozenset((25, 26, 27, 28, 29, 31, 32, 33, 38))
_HEADER = struct.Struct('<2sBBHHII')


class ClientError(Exception):
    """Connection or protocol failure; the connection has been closed."""


class ProtocolError(ClientError):
    pass


class CommandError(ClientError):
    def __init__(self, command, result):
        self.command, self.result = Command(command), result
        super().__init__('{}: {}'.format(self.command.name, RESULTS[result]))


def _field_id(field):
    if isinstance(field, str):
        return CONFIG_FIELDS.index(field)
    if type(field) is not int or not 0 <= field < len(CONFIG_FIELDS):
        raise ValueError('unknown config field')
    return field


def _float_bytes(value):
    value = float(value)
    if not math.isfinite(value):
        raise ValueError('finite float required')
    return struct.pack('<f', value)


class TrainingClient:
    """One serialized connection; no acquisition or motion on connect.

    Default heartbeat is 50 ms for the firmware's 250 ms default watchdog.
    Slow/blocked requests can still trip that watchdog: faults require explicit
    inspection/recovery. timeout bounds the entire request and cannot exceed 2 s.
    Any wire/command failure closes TCP and stops heartbeats. last_error exposes
    background failures; check_error() raises them in the caller's thread.
    """

    def __init__(self, host='192.168.11.2', port=39400, timeout=0.2,
                 heartbeat_interval=0.05):
        if not math.isfinite(timeout) or not 0 < timeout <= 2:
            raise ValueError('timeout must be in (0, 2] seconds')
        if not math.isfinite(heartbeat_interval) or not 0 < heartbeat_interval < 0.25:
            raise ValueError('heartbeat_interval must be in (0, 0.25) seconds')
        self.host, self.port = host, port
        self.timeout, self.heartbeat_interval = timeout, heartbeat_interval
        self._lock = threading.RLock()
        self._socket = None
        self._session = self._sequence = 0
        self._owned = False
        self._heartbeat_stop = threading.Event()
        self._heartbeat_stop.set()
        self.last_error = None

    @property
    def session(self):
        with self._lock:
            return self._session

    @property
    def connected(self):
        with self._lock:
            return self._socket is not None

    @property
    def acquired(self):
        with self._lock:
            return self._owned

    def check_error(self):
        with self._lock:
            if self.last_error is not None:
                raise self.last_error

    def _drop(self):
        self._heartbeat_stop.set()
        sock, self._socket = self._socket, None
        self._owned = False
        self._session = 0
        if sock is not None:
            sock.close()

    def close(self):
        """Disconnect only. Never sends Finish, Release or ClearFaults."""
        with self._lock:
            self._drop()

    disconnect = close

    def connect(self):
        with self._lock:
            if self._socket is not None:
                raise ClientError('already connected')
            try:
                self._socket = socket.create_connection((self.host, self.port), self.timeout)
                self._sequence = 0
                self._call(Command.HELLO, size=12,
                           decode=lambda b: struct.unpack('<III', b))
                self.last_error = None
                return self
            except Exception as exc:
                self.last_error = exc
                self._drop()
                raise

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    def _recv(self, count, deadline):
        data = bytearray()
        while len(data) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('request deadline exceeded')
            self._socket.settimeout(remaining)
            chunk = self._socket.recv(count - len(data))
            if not chunk:
                raise ConnectionError('peer disconnected')
            data.extend(chunk)
        return bytes(data)

    def _call(self, command, payload=b'', size=0, decode=None):
        command = Command(command)  # Reserved/unknown IDs can never reach TCP.
        if len(payload) > 256:
            raise ValueError('payload exceeds firmware limit')
        with self._lock:
            if self._socket is None:
                self.check_error()
                raise ClientError('connect explicitly first')
            try:
                if self._sequence == 0xffffffff:
                    raise ProtocolError('sequence exhausted; reconnect explicitly')
                self._sequence += 1
                head = _HEADER.pack(b'RH', 1, 0, len(payload), command,
                                    self._session, self._sequence)
                packet = head + payload
                deadline = time.monotonic() + self.timeout
                self._socket.settimeout(self.timeout)
                self._socket.sendall(packet + struct.pack('<I', zlib.crc32(packet)))
                head = self._recv(16, deadline)
                magic, version, flags, length, cmd, session, seq = _HEADER.unpack(head)
                if (magic != b'RH' or version != 1 or flags != 1 or
                        not 4 <= length <= 256 or cmd != command or seq != self._sequence):
                    raise ProtocolError('invalid response header')
                if command != Command.HELLO and session != self._session:
                    raise ProtocolError('response session mismatch')
                data = self._recv(length + 4, deadline)
                if zlib.crc32(head + data[:-4]) != struct.unpack('<I', data[-4:])[0]:
                    raise ProtocolError('response CRC mismatch')
                result, = struct.unpack('<I', data[:4])
                if result >= len(RESULTS):
                    raise ProtocolError('unknown result')
                if result:
                    if command == Command.HELLO and session:
                        raise ProtocolError('error Hello session mismatch')
                    if length != 4:
                        raise ProtocolError('invalid error response length')
                    raise CommandError(command, result)
                if length != size + 4:
                    raise ProtocolError('invalid response payload length')
                body = data[4:-4]
                if command == Command.HELLO:
                    if not session or struct.unpack('<III', body) != (1, 256, 0x1201):
                        raise ProtocolError('unsupported Hello contract')
                    self._session = session
                return decode(body) if decode else None
            except Exception as exc:
                self.last_error = exc
                self._drop()
                raise

    def acquire(self):
        with self._lock:
            self._call(Command.ACQUIRE)
            self._owned = True
            event = threading.Event()
            self._heartbeat_stop = event
            threading.Thread(target=self._heartbeats, args=(event,),
                             name='rehab-heartbeat', daemon=True).start()

    def _heartbeats(self, event):
        while not event.wait(self.heartbeat_interval):
            with self._lock:
                if event.is_set() or not self._owned:
                    return
                try:
                    self._call(Command.HEARTBEAT)
                except Exception:
                    return  # _call records failure and closes; never retry.

    def release(self):
        with self._lock:
            self._call(Command.RELEASE)
            self._heartbeat_stop.set()
            self._owned = False

    def enable(self): return self._call(Command.ENABLE)
    def pause(self): return self._call(Command.PAUSE)
    def resume(self): return self._call(Command.RESUME)
    def finish(self): return self._call(Command.FINISH)
    def stop(self): return self._call(Command.STOP)
    def clear_faults(self): return self._call(Command.CLEAR_FAULTS)
    def tare(self): return self._call(Command.TARE)
    def commit_config(self): return self._call(Command.COMMIT_CONFIG)

    def start(self, mode):
        if isinstance(mode, str):
            mode = Mode[mode.upper()]
        return self._call(Command.START, struct.pack('<I', Mode(mode)))

    def calibrate(self, force):
        return self._call(Command.CALIBRATE, _float_bytes(force))

    def status(self):
        names = ('state', 'mode', 'faults', 'session', 'epoch', 'config_valid',
                 'force_valid', 'sample_age_us', 'length_m', 'speed_m_s',
                 'force_n', 'target_torque_nm', 'target_enable', 'rfd_valid', 'motion_allowed')
        return self._call(Command.STATUS, size=60,
                          decode=lambda b: dict(zip(names, struct.unpack('<8I4f3I', b))))

    def diagnostics(self):
        """Return all 63 words in firmware diagnostic-index order."""
        return self._call(Command.DIAGNOSTICS, size=252,
                          decode=lambda b: struct.unpack('<63I', b))

    def statistics(self):
        names = ('repetitions', 'force_samples', 'elapsed_s', 'peak_force_n',
                 'mean_force_n', 'peak_speed_m_s', 'mean_speed_m_s', 'power_w',
                 'positive_energy_j', 'negative_energy_j', 'rfd_n_s',
                 'peak_rfd_n_s', 'fitness_result_valid')
        return self._call(Command.STATISTICS, size=52,
                          decode=lambda b: dict(zip(names, struct.unpack('<2I10fI', b))))

    def get_log(self):
        return self._call(Command.GET_LOG, size=16,
                          decode=lambda b: dict(zip(('found', 'timestamp', 'code', 'value'),
                                                    struct.unpack('<4I', b))))

    def get_config(self, first=0, count=None, staged=False):
        """Read contiguous fields; reads over 31 fields use multiple requests."""
        first = _field_id(first)
        count = len(CONFIG_FIELDS) - first if count is None else count
        if type(count) is not int or count <= 0 or first + count > len(CONFIG_FIELDS):
            raise ValueError('invalid config range')
        values = {}
        for start in range(first, first + count, 31):
            n = min(31, first + count - start)

            def decode(body):
                result = {}
                for offset, field in enumerate(range(start, start + n)):
                    ident, kind = struct.unpack_from('<HH', body, offset * 8)
                    if ident != field or kind != int(field in INTEGER_FIELDS):
                        raise ProtocolError('config field/type mismatch')
                    result[CONFIG_FIELDS[field]] = struct.unpack_from(
                        '<I' if kind else '<f', body, offset * 8 + 4)[0]
                return result

            values.update(self._call(Command.GET_CONFIG,
                                     struct.pack('<HHI', start, n, int(bool(staged))),
                                     size=8 * n, decode=decode))
        return values

    def set_config(self, values=None, **fields):
        """Stage named fields (or integer IDs). Commit explicitly; no persistence."""
        items = dict(values or {})
        items.update(fields)
        if not 1 <= len(items) <= 32:
            raise ValueError('set 1..32 fields per request')
        payload = bytearray()
        seen = set()
        for name, value in items.items():
            field = _field_id(name)
            if field in seen:
                raise ValueError('duplicate config field')
            seen.add(field)
            integer = field in INTEGER_FIELDS
            if integer and (type(value) is not int or not 0 <= value <= 0xffffffff):
                raise ValueError('uint32 required for ' + CONFIG_FIELDS[field])
            payload.extend(struct.pack('<HH', field, int(integer)))
            payload.extend(struct.pack('<I', value) if integer else _float_bytes(value))
        return self._call(Command.SET_CONFIG, bytes(payload))

    def set_network(self, ip, gateway, netmask, port=39400):
        """Stage firmware's asynchronous network update; it may disconnect TCP."""
        payload = b''.join(ipaddress.IPv4Address(v).packed for v in (ip, gateway, netmask))
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError('port must be 1..65535')
        return self._call(Command.SET_NETWORK, payload + struct.pack('<H', port))
