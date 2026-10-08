// End-to-end host tests of the production framing, replay, service and Engine.
// No HAL mocks or second implementation of training algorithms are used.
#include "../../Rehab/protocol/service.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

#ifndef REHAB_ENABLE_APPLICATION
#define REHAB_ENABLE_APPLICATION 1
#endif

using namespace rehab;
using namespace rehab::wire;
namespace {
uint64_t checks = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " #expression "\n"; std::exit(1); } } while (0)

bool equal_frame(const Frame& a, const Frame& b) {
    return a.flags == b.flags && a.command == b.command && a.length == b.length &&
           a.session == b.session && a.sequence == b.sequence &&
           std::memcmp(a.payload, b.payload, a.length) == 0;
}
Result result_of(const Frame& frame) {
    CHECK(frame.flags == 1 && frame.length >= 4 && frame.length <= kPayload);
    const uint32_t value = u32(frame.payload);
    CHECK(value <= static_cast<uint32_t>(Result::Inhibited));
    return static_cast<Result>(value);
}
[[maybe_unused]] uint32_t float_bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
[[maybe_unused]] float read_float(const uint8_t* data) {
    const uint32_t bits = u32(data);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
Config settings() {
    Config c;
    c.radius_m = 0.02f;
    c.motor_turns_per_drum_turn = 2;
    c.efficiency = 0.8f;
    c.force_scale_n_per_count = 0.001f;
    c.min_length_m = 0;
    c.max_length_m = 2;
    c.max_speed_m_s = 1;
    c.max_force_n = 200;
    c.max_torque_nm = 2;
    c.torque_slew_nm_s = 2;
    c.integrator_limit_nm = 0.5f;
    c.concentric_force_n = 20;
    c.eccentric_force_n = 10;
    c.speed_limit_m_s = 0.4f;
    c.return_speed_m_s = 0.2f;
    c.elastic_n_m = 100;
    c.water_n_s2_m2 = 50;
    c.phase_min_distance_m = 0.002f;
    c.phase_dwell_ms = 10;
    return c;
}

struct Fixture {
    Engine engine;
    Hooks hooks;
    Service service;
    Parser requests[3];
    Parser responses;
    Feedback feedback{};
    ForceSample sample{};
    Config config = settings();
    Frame last_response{};
    uint32_t sessions[3]{}, sequences[3]{};
    uint32_t next_session = 1000;
    uint64_t total_us = 0;
    uint32_t now = 0;
    unsigned route = 0, response_count = 0;
    unsigned acquire_count = 0, release_count = 0, clear_count = 0;
    unsigned network_count = 0, log_count = 0, emergency_count = 0;
    unsigned execution_counts[22]{};
    bool motion_allowed = true;
    bool offer_log = true;
    float length = 0.5f, speed = 0;
    int32_t raw = 10000;

    static Hooks make_hooks(Fixture* fixture) {
        Hooks h;
        h.ctx = fixture;
        h.new_session = [](void* context) {
            auto& f = *static_cast<Fixture*>(context);
            return ++f.next_session;
        };
        h.acquire = [](void* context, uint32_t session, const Feedback& feedback) {
            auto& f = *static_cast<Fixture*>(context);
            ++f.acquire_count;
            return f.engine.acquire(session, feedback);
        };
        h.release = [](void* context, uint32_t session, const Feedback& feedback) {
            auto& f = *static_cast<Fixture*>(context);
            ++f.release_count;
            return f.engine.release(session, feedback);
        };
        h.motion_allowed = [](void* context) { return static_cast<Fixture*>(context)->motion_allowed; };
        h.clear_faults = [](void* context, uint32_t session) {
            auto& f = *static_cast<Fixture*>(context);
            ++f.clear_count;
            return f.engine.clear_faults(session);
        };
        h.network = [](void* context, const uint8_t*, size_t length) {
            auto& f = *static_cast<Fixture*>(context);
            ++f.network_count;
            return length == 12 ? Result::Ok : Result::InvalidConfig;
        };
        h.diagnostic = [](void*, unsigned index) { return 0xAB000000u + index; };
        h.read_log = [](void* context, uint32_t& timestamp, uint32_t& code, uint32_t& value) {
            auto& f = *static_cast<Fixture*>(context);
            if (!f.offer_log) return false;
            f.offer_log = false;
            timestamp = 12345; code = 0x53; value = 0xFF;
            return true;
        };
        h.command_log = [](void* context, uint16_t command, Result) {
            auto& f = *static_cast<Fixture*>(context);
            ++f.log_count;
            if (command < 22) ++f.execution_counts[command];
        };
        h.emergency_stop = [](void* context) { ++static_cast<Fixture*>(context)->emergency_count; };
        return h;
    }
    Fixture() : hooks(make_hooks(this)), service(engine, hooks) {}
    static void receive_response(void* context, const Frame& frame) {
        auto& f = *static_cast<Fixture*>(context);
        ++f.response_count;
        f.last_response = frame;
    }
    static void receive_request(void* context, const Frame& request) {
        auto& f = *static_cast<Fixture*>(context);
        Frame response;
        f.service.handle(f.route, request, response, f.now, f.feedback);
        uint8_t bytes[kMaxFrame];
        const size_t length = encode(response, bytes, sizeof(bytes));
        CHECK(length != 0);
        // A distinct response parser verifies wire lengths, payload and CRC.
        f.responses.consume(bytes, length, static_cast<uint32_t>(f.total_us / 1000u), receive_response, &f);
    }
    Frame exchange(unsigned port, const Frame& request) {
        CHECK(port < 3);
        uint8_t bytes[kMaxFrame];
        const size_t length = encode(request, bytes, sizeof(bytes));
        CHECK(length != 0);
        route = port;
        const unsigned before = response_count;
        const size_t chunks[] = {1, 3, 11, 67};
        size_t offset = 0, index = 0;
        while (offset < length) {
            const size_t amount = std::min(chunks[index++ % 4], length - offset);
            requests[port].consume(bytes + offset, amount, static_cast<uint32_t>(total_us / 1000u), receive_request, this);
            offset += amount;
        }
        CHECK(response_count == before + 1);
        CHECK(last_response.command == request.command && last_response.sequence == request.sequence);
        return last_response;
    }
    Frame request(unsigned port, Command command) {
        Frame frame;
        frame.command = static_cast<uint16_t>(command);
        frame.session = sessions[port];
        frame.sequence = ++sequences[port];
        return frame;
    }
    Frame hello(unsigned port = 0) {
        Frame request_frame;
        request_frame.command = static_cast<uint16_t>(Command::Hello);
        const Frame response = exchange(port, request_frame);
        CHECK(result_of(response) == Result::Ok);
        CHECK(response.session != 0 && response.length == 16);
        CHECK(u32(response.payload + 4) == kVersion && u32(response.payload + 8) == kPayload);
        sessions[port] = response.session;
        sequences[port] = 0;
        return response;
    }
    Frame send(unsigned port, Command command) { return exchange(port, request(port, command)); }
    Result command(Command command_value, unsigned port = 0) { return result_of(send(port, command_value)); }
    Result start(Mode mode = Mode::Isotonic, unsigned port = 0) {
        Frame frame = request(port, Command::Start);
        frame.length = 4;
        put32(frame.payload, static_cast<uint32_t>(mode));
        return result_of(exchange(port, frame));
    }
    Result set_field(uint16_t id, uint16_t type, uint32_t value, unsigned port = 0) {
        Frame frame = request(port, Command::SetConfig);
        frame.length = 8;
        put16(frame.payload, id); put16(frame.payload + 2, type); put32(frame.payload + 4, value);
        return result_of(exchange(port, frame));
    }
    void stage_config(const Config& candidate, unsigned port = 0) {
        for (uint16_t first = 0; first < config_field_count(); first += 11) {
            Frame frame = request(port, Command::SetConfig);
            for (uint16_t id = first; id < std::min<size_t>(first + 11, config_field_count()); ++id) {
                uint16_t type = 0; uint32_t value = 0;
                CHECK(get_config_field(candidate, id, type, value));
                put16(frame.payload + frame.length, id);
                put16(frame.payload + frame.length + 2, type);
                put32(frame.payload + frame.length + 4, value);
                frame.length += 8;
            }
            CHECK(result_of(exchange(port, frame)) == Result::Ok);
        }
    }
    Frame get_fields(uint16_t first, uint16_t count, bool staged, unsigned port = 0) {
        Frame frame = request(port, Command::GetConfig);
        frame.length = 8;
        put16(frame.payload, first); put16(frame.payload + 2, count); put32(frame.payload + 4, staged ? 1u : 0u);
        return exchange(port, frame);
    }
    uint32_t get_field(uint16_t id, bool staged, unsigned port = 0) {
        const auto frame = get_fields(id, 1, staged, port);
        CHECK(result_of(frame) == Result::Ok && frame.length == 12);
        CHECK(u16(frame.payload + 4) == id);
        return u32(frame.payload + 8);
    }
    void update(bool fresh) {
        feedback.timestamp_us = now;
        const float scale = config.direction * 6.28318530718f * config.radius_m /
                            config.motor_turns_per_drum_turn / config.encoder_turns_per_motor_turn;
        feedback.position_turns = length / scale + config.zero_turns;
        feedback.velocity_turns_s = speed / scale;
        if (fresh) sample = {sample.sequence + 1u, now, raw, true};
        engine.update(now, feedback, sample);
    }
    void advance(unsigned milliseconds, bool heartbeat = true, unsigned port = 0) {
        for (unsigned i = 0; i < milliseconds; ++i) {
            total_us += 1000;
            now = static_cast<uint32_t>(total_us);
            feedback.armed = engine.status().target.enable;
            feedback.idle = !feedback.armed;
            length += speed * 0.001f;
            update(total_us % 13000 == 0);
            if (heartbeat && engine.status().session && total_us % 50000 == 0)
                CHECK(command(Command::Heartbeat, port) == Result::Ok);
        }
    }
    void prepare() {
        hello();
        stage_config(config);
        CHECK(!engine.status().config_valid);
        CHECK(command(Command::CommitConfig) == Result::Ok);
        CHECK(engine.status().config_valid);
        update(true);
        CHECK(command(Command::Acquire) == Result::Ok);
        CHECK(service.owner_transport() == 0);
        CHECK(command(Command::Heartbeat) == Result::Ok);
    }
};

void read_only_and_transport_guards() {
    Fixture f;
    const auto first = f.hello(0), second = f.hello(1), third = f.hello(2);
    CHECK(first.session != second.session && second.session != third.session && first.session != third.session);
    const auto status = f.send(1, Command::Status);
    CHECK(result_of(status) == Result::Ok && status.length == 64);
    CHECK(u32(status.payload + 4) == static_cast<uint32_t>(State::Standby));
    CHECK(u32(status.payload + 24) == 0 && u32(status.payload + 60) == 1);
    f.motion_allowed = false;
    CHECK(u32(f.send(1, Command::Status).payload + 60) == 0);
    const auto diagnostics = f.send(2, Command::Diagnostics);
    CHECK(result_of(diagnostics) == Result::Ok && diagnostics.length == 4+4*kDiagnosticWords);
    for (unsigned i = 0; i < kDiagnosticWords; ++i) CHECK(u32(diagnostics.payload + 4 + i * 4) == 0xAB000000u + i);
    Frame log_request = f.request(2, Command::GetLog);
    const auto log = f.exchange(2, log_request);
    CHECK(result_of(log) == Result::Ok && log.length == 20 && u32(log.payload + 4) == 1);
    CHECK(u32(log.payload + 8) == 12345 && u32(log.payload + 12) == 0x53 && u32(log.payload + 16) == 0xFF);
    CHECK(equal_frame(log, f.exchange(2, log_request)));
    CHECK(u32(f.send(2, Command::GetLog).payload + 4) == 0);
    Frame forged = f.request(1, Command::Status);
    forged.session = f.sessions[0];
    CHECK(result_of(f.exchange(1, forged)) == Result::BadSequence);
    Frame invalid = f.request(0, Command::Status), response;
    f.service.handle(3, invalid, response, 0, f.feedback);
    CHECK(result_of(response) == Result::InvalidConfig);
    invalid.flags = 1;
    CHECK(result_of(f.exchange(0, invalid)) == Result::InvalidConfig);
    invalid.flags = 0; invalid.length = static_cast<uint16_t>(kPayload + 1);
    f.service.handle(0, invalid, response, 0, f.feedback);
    CHECK(result_of(response) == Result::InvalidConfig);
    invalid = f.request(0, Command::Hello); // HELLO must have session zero.
    CHECK(result_of(f.exchange(0, invalid)) == Result::InvalidConfig);
    Frame network = f.request(0, Command::SetNetwork);
    network.length = 12;
    const auto network_reply = f.exchange(0, network);
    CHECK(result_of(network_reply) == Result::Ok);
    CHECK(f.network_count == 1);
    CHECK(equal_frame(network_reply, f.exchange(0, network)) && f.network_count == 1);
#if REHAB_ENABLE_APPLICATION && REHAB_ENABLE_STATISTICS
    CHECK(f.command(Command::Statistics, 1) == Result::Ok);
#else
    CHECK(f.command(Command::Statistics, 1) == Result::Unsupported);
#endif
}

#if REHAB_ENABLE_APPLICATION
void configuration_validation() {
    Fixture f;
    f.hello();
    f.stage_config(f.config);
    CHECK(!f.engine.status().config_valid);
    CHECK(f.get_field(0, true) == float_bits(f.config.radius_m));
    CHECK(f.get_field(0, false) == float_bits(0));
    CHECK(f.command(Command::CommitConfig) == Result::Ok);
    CHECK(f.get_field(0, false) == float_bits(f.config.radius_m));
    const auto maximum_read = f.get_fields(0, 31, false);
    CHECK(result_of(maximum_read) == Result::Ok && maximum_read.length == 252);
    for (uint16_t id = 0; id < 31; ++id) {
        uint16_t type = 0; uint32_t bits = 0;
        CHECK(get_config_field(f.config, id, type, bits));
        CHECK(u16(maximum_read.payload + 4 + id * 8) == id);
        CHECK(u16(maximum_read.payload + 6 + id * 8) == type);
        CHECK(u32(maximum_read.payload + 8 + id * 8) == bits);
    }
    CHECK(f.set_field(0, 0, float_bits(std::numeric_limits<float>::quiet_NaN())) == Result::InvalidConfig);
    CHECK(f.set_field(0, 0, float_bits(std::numeric_limits<float>::infinity())) == Result::InvalidConfig);
    CHECK(f.set_field(0, 1, 17) == Result::InvalidConfig);
    CHECK(f.set_field(25, 0, float_bits(50)) == Result::InvalidConfig);
    CHECK(f.set_field(static_cast<uint16_t>(config_field_count()), 0, 0) == Result::InvalidConfig);
    CHECK(f.get_field(0, true) == float_bits(f.config.radius_m));

    Frame atomic = f.request(0, Command::SetConfig);
    atomic.length = 16;
    put16(atomic.payload, 0); put16(atomic.payload + 2, 0); put32(atomic.payload + 4, float_bits(0.03f));
    put16(atomic.payload + 8, UINT16_MAX); put16(atomic.payload + 10, 0); put32(atomic.payload + 12, 0);
    CHECK(result_of(f.exchange(0, atomic)) == Result::InvalidConfig);
    CHECK(f.get_field(0, true) == float_bits(f.config.radius_m));

    CHECK(f.set_field(0, 0, float_bits(-1)) == Result::Ok); // Cross-field/range validation is at commit.
    CHECK(f.command(Command::CommitConfig) == Result::InvalidConfig);
    CHECK(f.engine.config().radius_m == f.config.radius_m);
    CHECK(f.set_field(0, 0, float_bits(f.config.radius_m)) == Result::Ok);
    CHECK(f.set_field(26, 1, UINT32_MAX) == Result::Ok);
    CHECK(f.command(Command::CommitConfig) == Result::InvalidConfig);
    CHECK(f.engine.config().heartbeat_timeout_ms == f.config.heartbeat_timeout_ms);
    CHECK(f.set_field(26, 1, f.config.heartbeat_timeout_ms) == Result::Ok);
    CHECK(f.set_field(38, 1, 0x80000000u) == Result::Ok);
    CHECK(f.command(Command::CommitConfig) == Result::InvalidConfig);
    CHECK(f.set_field(38, 1, 0) == Result::Ok);
    CHECK(f.command(Command::CommitConfig) == Result::Ok);
#if !REHAB_ENABLE_COMPENSATION
    CHECK(f.set_field(34, 0, float_bits(0.05f)) == Result::Ok);
    CHECK(f.set_field(37, 0, float_bits(0.1f)) == Result::Ok);
    CHECK(f.set_field(38, 1, FrictionEnabled | CompensationIdentified) == Result::Ok);
    CHECK(f.command(Command::CommitConfig) == Result::InvalidConfig);
    CHECK(f.engine.config().flags == 0);
    CHECK(f.set_field(38, 1, 0) == Result::Ok);
    CHECK(f.command(Command::CommitConfig) == Result::Ok);
#endif

    Frame wrong = f.request(0, Command::SetConfig);
    CHECK(result_of(f.exchange(0, wrong)) == Result::InvalidConfig);
    wrong = f.request(0, Command::SetConfig); wrong.length = 7;
    CHECK(result_of(f.exchange(0, wrong)) == Result::InvalidConfig);
    wrong = f.request(0, Command::CommitConfig); wrong.length = 1;
    CHECK(result_of(f.exchange(0, wrong)) == Result::InvalidConfig);
    CHECK(result_of(f.get_fields(0, 0, true)) == Result::InvalidConfig);
    CHECK(result_of(f.get_fields(0, 32, true)) == Result::InvalidConfig);
    CHECK(result_of(f.get_fields(38, 2, true)) == Result::InvalidConfig);
    CHECK(result_of(f.get_fields(UINT16_MAX, 2, true)) == Result::InvalidConfig);
    wrong = f.request(0, Command::GetConfig); wrong.length = 8;
    put16(wrong.payload + 2, 1); put32(wrong.payload + 4, 2);
    CHECK(result_of(f.exchange(0, wrong)) == Result::InvalidConfig);
    wrong = f.request(0, Command::GetConfig); wrong.length = 4;
    CHECK(result_of(f.exchange(0, wrong)) == Result::InvalidConfig);
    wrong = f.request(0, Command::Status); wrong.length = 1;
    CHECK(result_of(f.exchange(0, wrong)) == Result::InvalidConfig);

    // Old sequence outside the four-entry reply cache cannot execute again.
    Frame old = f.request(0, Command::Status);
    CHECK(result_of(f.exchange(0, old)) == Result::Ok);
    for (unsigned i = 0; i < 5; ++i) CHECK(f.command(Command::Status) == Result::Ok);
    const unsigned executions = f.log_count;
    CHECK(result_of(f.exchange(0, old)) == Result::BadSequence);
    CHECK(f.log_count == executions);
    Frame reused = f.request(0, Command::Status);
    CHECK(result_of(f.exchange(0, reused)) == Result::Ok);
    reused.command = static_cast<uint16_t>(Command::Diagnostics);
    CHECK(result_of(f.exchange(0, reused)) == Result::BadSequence);
}

void calibration_and_staging() {
    Fixture f;
    f.hello();
    Config mechanical = f.config;
    mechanical.radius_m = 0.033f;
    mechanical.force_scale_n_per_count = 0;
    f.stage_config(mechanical);
    CHECK(f.command(Command::CommitConfig) == Result::InvalidConfig);
    CHECK(f.command(Command::Acquire) == Result::Ok);
    CHECK(f.command(Command::Enable) == Result::InvalidConfig);
    f.raw = 1000;
    f.advance(104);
    Frame tare = f.request(0, Command::Tare);
    const auto tare_reply = f.exchange(0, tare);
    CHECK(result_of(tare_reply) == Result::Ok);
    CHECK(!f.engine.status().config_valid);
    const unsigned tare_count = f.execution_counts[static_cast<unsigned>(Command::Tare)];
    f.raw = 3000; f.advance(13, false);
    CHECK(equal_frame(tare_reply, f.exchange(0, tare)));
    CHECK(f.engine.config().force_offset_counts == 1000);
    CHECK(f.execution_counts[static_cast<unsigned>(Command::Tare)] == tare_count);
    CHECK(f.get_field(0, true) == float_bits(0.033f));
    CHECK(f.get_field(7, true) == float_bits(1000));
    CHECK(f.command(Command::Enable) == Result::InvalidConfig);

    f.raw = 11000;
    f.advance(117);
    Frame bad = f.request(0, Command::Calibrate);
    CHECK(result_of(f.exchange(0, bad)) == Result::InvalidConfig);
    bad = f.request(0, Command::Calibrate); bad.length = 4;
    put32(bad.payload, float_bits(std::numeric_limits<float>::quiet_NaN()));
    CHECK(result_of(f.exchange(0, bad)) == Result::InvalidConfig);
    Frame calibrate = f.request(0, Command::Calibrate); calibrate.length = 4;
    put32(calibrate.payload, float_bits(10));
    const auto calibration_reply = f.exchange(0, calibrate);
    CHECK(result_of(calibration_reply) == Result::Ok);
    CHECK(!f.engine.status().config_valid);
    const unsigned calibration_count = f.execution_counts[static_cast<unsigned>(Command::Calibrate)];
    f.raw = 21000; f.advance(13, false);
    CHECK(equal_frame(calibration_reply, f.exchange(0, calibrate)));
    CHECK(f.execution_counts[static_cast<unsigned>(Command::Calibrate)] == calibration_count);
    CHECK(std::fabs(f.engine.config().force_scale_n_per_count - 0.001f) < 1e-7f);
    CHECK(f.get_field(0, true) == float_bits(0.033f));
    CHECK(f.get_field(6, true) == float_bits(f.engine.config().force_scale_n_per_count));
    CHECK(f.command(Command::CommitConfig) == Result::Ok);
    f.config = f.engine.config();
    CHECK(f.engine.config().radius_m == 0.033f && f.engine.config().force_offset_counts == 1000);
    CHECK(f.command(Command::Enable) == Result::Inhibited); // Commit invalidates the old conversion.
    f.advance(13);
    CHECK(f.command(Command::Enable) == Result::Ok);
}

void ownership_and_feature_switches() {
    Fixture f;
    f.prepare();
    f.hello(1);
    CHECK(f.command(Command::Acquire, 1) == Result::Busy);
    CHECK(f.command(Command::Enable, 1) == Result::NotOwner);
    CHECK(f.command(Command::Heartbeat, 1) == Result::NotOwner);
    CHECK(f.set_field(0, 0, float_bits(0.05f), 1) == Result::Busy);
    CHECK(f.command(Command::CommitConfig, 1) == Result::Busy);
    Frame forged = f.request(1, Command::Enable); forged.session = f.sessions[0];
    CHECK(result_of(f.exchange(1, forged)) == Result::BadSequence);
    CHECK(!f.engine.status().target.enable);
    Frame network = f.request(0, Command::SetNetwork); network.length = 12;
    CHECK(result_of(f.exchange(0, network)) == Result::Busy && f.network_count == 0);
    Frame hello; hello.command = static_cast<uint16_t>(Command::Hello);
    CHECK(result_of(f.exchange(0, hello)) == Result::Busy);
    f.motion_allowed = false;
    CHECK(f.command(Command::Enable) == Result::Inhibited);
    CHECK(f.start() == Result::Inhibited);
    CHECK(f.command(Command::Resume) == Result::Inhibited);
    CHECK(f.engine.status().state == State::Standby && !f.engine.status().target.enable);
    f.motion_allowed = true;
    CHECK(f.command(Command::Enable) == Result::Ok);
    CHECK(!f.engine.status().target.enable);
    Frame unknown = f.request(0, Command::Status); unknown.command = UINT16_MAX;
    CHECK(result_of(f.exchange(0, unknown)) == Result::Unsupported);
    CHECK(f.start(Mode::Release) == Result::Unsupported);
    CHECK(f.command(Command::Start) == Result::InvalidConfig);
    Frame bad = f.request(0, Command::Start); bad.length = 4; put32(bad.payload, 999);
    CHECK(result_of(f.exchange(0, bad)) == Result::InvalidConfig);
#if REHAB_ENABLE_TRAINING && !REHAB_ENABLE_STATISTICS
    CHECK(f.start(Mode::Fitness) == Result::Unsupported);
#endif
#if !REHAB_ENABLE_TRAINING
    CHECK(f.start() == Result::Unsupported);
    CHECK(f.command(Command::Resume) == Result::Unsupported);
#else
    CHECK(f.start() == Result::Ok);
    f.advance(100);
    CHECK(f.engine.status().state == State::Running);
#endif
#if REHAB_ENABLE_STATISTICS
    CHECK(f.command(Command::Statistics) == Result::Ok);
#else
    CHECK(f.command(Command::Statistics) == Result::Unsupported);
    CHECK(!f.engine.status().rfd_valid);
#endif
}

#if REHAB_ENABLE_TRAINING
void complete_lifecycle_and_replay() {
    Fixture f;
    f.prepare();
    CHECK(f.command(Command::Enable) == Result::Ok);
    Frame start = f.request(0, Command::Start); start.length = 4;
    put32(start.payload, static_cast<uint32_t>(Mode::Isotonic));
    const auto reply = f.exchange(0, start);
    CHECK(result_of(reply) == Result::Ok);
    f.advance(40, false);
    const auto before = f.engine.status();
    const unsigned executions = f.execution_counts[static_cast<unsigned>(Command::Start)];
    CHECK(equal_frame(reply, f.exchange(0, start)));
    CHECK(f.engine.status().state == State::Running);
    CHECK(f.engine.status().target.sequence == before.target.sequence);
    CHECK(f.engine.status().stats.elapsed_s == before.stats.elapsed_s);
    CHECK(f.execution_counts[static_cast<unsigned>(Command::Start)] == executions);
    f.advance(160);
    CHECK(f.engine.status().target.enable && f.engine.status().target.torque_nm < 0);
    CHECK(f.command(Command::Release) == Result::Busy);
    CHECK(f.command(Command::Tare) == Result::InvalidState);
    CHECK(f.command(Command::CommitConfig) == Result::Busy);
    CHECK(f.command(Command::Pause) == Result::Ok);
    f.advance(150);
    CHECK(f.engine.status().state == State::Paused && !f.engine.status().target.enable);
    CHECK(f.engine.status().session == f.sessions[0]);
    f.motion_allowed = false;
    CHECK(f.command(Command::Resume) == Result::Inhibited);
    f.motion_allowed = true;
    CHECK(f.command(Command::Resume) == Result::Ok);
    f.advance(200);
    CHECK(f.engine.status().state == State::Running);
    const auto status = f.send(0, Command::Status);
    CHECK(u32(status.payload + 4) == static_cast<uint32_t>(State::Running));
    CHECK(u32(status.payload + 16) == f.sessions[0]);
    CHECK(read_float(status.payload + 44) > 0 && read_float(status.payload + 48) < 0);
    CHECK(u32(status.payload + 52) == 1);
#if REHAB_ENABLE_STATISTICS
    const auto stats = f.send(0, Command::Statistics);
    CHECK(result_of(stats) == Result::Ok && stats.length == 56);
    CHECK(u32(stats.payload + 8) > 0 && read_float(stats.payload + 12) > 0);
#endif
    CHECK(f.command(Command::Finish) == Result::Ok);
    f.advance(150);
    CHECK(f.engine.status().state == State::Standby && !f.engine.status().target.enable);
    CHECK(f.engine.status().session == f.sessions[0]);
    CHECK(f.command(Command::Release) == Result::Ok);
    CHECK(f.engine.status().session == 0 && f.service.owner_transport() == 3);
    CHECK(f.acquire_count == 1 && f.release_count == 2); // One busy attempt while armed, then success.
    CHECK(f.command(Command::Resume) == Result::NotOwner);
}

void observer_emergency_stop() {
    Fixture f;
    f.prepare(); f.hello(1);
    CHECK(f.command(Command::Enable) == Result::Ok);
    CHECK(f.start() == Result::Ok); f.advance(100);
    CHECK(result_of(f.send(1, Command::Finish)) == Result::NotOwner);
    const Frame stop = f.request(1, Command::Stop);
    const auto response = f.exchange(1, stop);
    CHECK(result_of(response) == Result::Ok);
    CHECK(f.emergency_count == 1);
    CHECK(f.engine.status().faults & FaultStop);
    CHECK(!f.engine.status().target.enable);
    CHECK(equal_frame(response, f.exchange(1, stop)));
    CHECK(f.emergency_count == 1);
    CHECK(f.command(Command::Resume) == Result::Faulted);
}

void duplicate_heartbeat_does_not_refresh() {
    Fixture f;
    f.prepare();
    CHECK(f.command(Command::Enable) == Result::Ok);
    CHECK(f.start() == Result::Ok);
    const Frame heartbeat = f.request(0, Command::Heartbeat);
    const auto reply = f.exchange(0, heartbeat);
    CHECK(result_of(reply) == Result::Ok);
    const auto count = f.execution_counts[static_cast<unsigned>(Command::Heartbeat)];
    for (unsigned i = 0; i < 5; ++i) {
        f.advance(50, false);
        CHECK(equal_frame(reply, f.exchange(0, heartbeat)));
    }
    CHECK(f.engine.status().state == State::Fault);
    CHECK(f.engine.status().faults & FaultHeartbeat);
    CHECK(!f.engine.status().target.enable);
    CHECK(f.execution_counts[static_cast<unsigned>(Command::Heartbeat)] == count);
    // A new heartbeat also cannot clear a latched timeout or restart training.
    CHECK(f.command(Command::Heartbeat) == Result::Ok);
    CHECK(f.engine.status().state == State::Fault);
}

void disconnected_lease_recovery() {
    Fixture f;
    f.prepare();
    f.hello(1);
    CHECK(f.command(Command::Enable) == Result::Ok);
    CHECK(f.start() == Result::Ok);
    f.advance(200);
    const auto old_session = f.sessions[0];
    const auto epoch = f.engine.status().epoch;
    f.service.disconnect(1); // An observer disappearing must not stop the owner.
    CHECK(f.engine.status().state == State::Running);
    f.service.disconnect(0);
    CHECK(f.engine.status().state == State::Fault && (f.engine.status().faults & FaultHeartbeat));
    CHECK(!f.engine.status().target.enable && f.engine.status().session == old_session);
    f.hello(0);
    CHECK(f.sessions[0] != old_session);
    CHECK(f.command(Command::Acquire) == Result::Busy);
    CHECK(f.command(Command::Resume) == Result::NotOwner);
    CHECK(f.command(Command::ClearFaults) == Result::NotOwner); // Feedback still reports armed.
    f.advance(1, false);
    CHECK(f.feedback.idle && !f.feedback.armed);
    f.hello(1);
    CHECK(f.command(Command::ClearFaults, 1) == Result::NotOwner);
    f.feedback.stop = true; f.update(false);
    CHECK(f.command(Command::ClearFaults) == Result::NotOwner);
    f.feedback.stop = false; f.update(false);
    CHECK(f.command(Command::ClearFaults) == Result::Ok);
    CHECK(f.clear_count == 1 && f.release_count == 1);
    CHECK(f.engine.status().session == 0 && f.service.owner_transport() == 3);
    CHECK(f.engine.status().state == State::Standby && f.engine.status().faults == 0);
    CHECK(f.engine.status().epoch != epoch && !f.engine.status().target.enable);
    CHECK(f.command(Command::Resume) == Result::NotOwner);
    CHECK(f.command(Command::Acquire) == Result::Ok);
    CHECK(f.command(Command::Resume) == Result::InvalidState);
    CHECK(f.start() == Result::InvalidState);
    CHECK(!f.engine.status().target.enable);
    CHECK(f.command(Command::Enable) == Result::Ok);
    CHECK(f.start() == Result::Ok);
}

void long_virtual_run() {
    Fixture f;
    f.prepare();
    CHECK(f.command(Command::Enable) == Result::Ok);
    Frame old_start = f.request(0, Command::Start); old_start.length = 4;
    put32(old_start.payload, static_cast<uint32_t>(Mode::Isotonic));
    CHECK(result_of(f.exchange(0, old_start)) == Result::Ok);
    uint32_t rng = 0x52454841u;
    uint32_t wraps = 0, previous = 0;
    double reference_positive_j = 0.0, reference_negative_j = 0.0;
    double reference_force_sum = 0.0;
    constexpr uint64_t duration_us = 24ull * 60 * 60 * 1000000;
    for (f.total_us = 10000; f.total_us <= duration_us; f.total_us += 10000) {
        f.now = static_cast<uint32_t>(f.total_us);
        if (f.now < previous) ++wraps;
        previous = f.now;
        const auto phase = f.total_us % 4000000;
        f.speed = phase < 2000000 ? 0.2f : -0.2f;
        f.length = phase < 2000000 ? 0.3f + phase * 0.0000002f : 0.7f - (phase - 2000000) * 0.0000002f;
        f.feedback.armed = f.engine.status().target.enable;
        f.feedback.idle = !f.feedback.armed;
        rng = rng * 1664525u + 1013904223u;
        f.raw = 10000 + static_cast<int32_t>((rng >> 24) & 15u);
        f.update(f.total_us % 100000 == 0); // Real 10 SPS sample timestamps.
        const double reference_power = static_cast<double>(f.engine.status().force_n) * f.speed;
        if (reference_power >= 0) reference_positive_j += reference_power * 0.01;
        else reference_negative_j -= reference_power * 0.01;
        if (f.total_us % 100000 == 0) reference_force_sum += f.engine.status().force_n;
        CHECK(f.engine.status().state == State::Running);
        CHECK(std::isfinite(f.engine.status().target.torque_nm));
        CHECK(std::fabs(f.engine.status().target.torque_nm) <= f.config.max_torque_nm);
        if (f.speed < 0) CHECK(f.engine.status().target.torque_nm == 0);
        if (f.total_us % 100000 == 0) CHECK(f.command(Command::Heartbeat) == Result::Ok);
    }
    CHECK(wraps == 20);
#if REHAB_ENABLE_STATISTICS
    CHECK(f.engine.status().stats.repetitions >= 21598 && f.engine.status().stats.repetitions <= 21600);
    CHECK(f.engine.status().stats.force_samples == 864000);
    CHECK(f.engine.status().stats.positive_energy_j > 0 && f.engine.status().stats.negative_energy_j > 0);
    CHECK(f.engine.status().rfd_valid);
    CHECK(std::fabs(f.engine.status().stats.elapsed_s - 86400.0f) <= 1.0f);
    // A 2 ppm relative tolerance includes final float telemetry quantization.
    CHECK(std::fabs(f.engine.status().stats.positive_energy_j - reference_positive_j) / reference_positive_j <= 2e-6);
    CHECK(std::fabs(f.engine.status().stats.negative_energy_j - reference_negative_j) / reference_negative_j <= 2e-6);
    CHECK(std::fabs(f.engine.status().stats.mean_speed_m_s - 0.2f) <= 1e-6f);
    CHECK(std::fabs(f.engine.status().stats.mean_force_n - reference_force_sum / 864000) <= 2e-5);
#else
    CHECK(f.engine.status().stats.elapsed_s == 0 && !f.engine.status().rfd_valid);
    (void)reference_positive_j; (void)reference_negative_j; (void)reference_force_sum;
#endif
    std::cout << "24 h virtual service/core run: clock wraps=" << wraps
              << ", samples=" << f.engine.status().stats.force_samples
              << ", repetitions=" << f.engine.status().stats.repetitions
              << ", reported seconds=" << f.engine.status().stats.elapsed_s << '\n';
#if REHAB_ENABLE_STATISTICS
    std::cout << "24 h energy relative error: positive="
              << std::fabs(f.engine.status().stats.positive_energy_j - reference_positive_j) / reference_positive_j
              << ", negative="
              << std::fabs(f.engine.status().stats.negative_energy_j - reference_negative_j) / reference_negative_j << '\n';
#endif
    // Inject a STOP after the long run and prove delayed/replayed Start cannot
    // clear it. State recovery is through explicit Clear -> Enable -> Start.
    f.total_us = duration_us + 1000;
    f.now = static_cast<uint32_t>(f.total_us);
    f.feedback.stop = true;
    f.update(false);
    CHECK(f.engine.status().faults & FaultStop);
    CHECK(!f.engine.status().target.enable);
    CHECK(result_of(f.exchange(0, old_start)) == Result::BadSequence);
    CHECK(f.start() == Result::Faulted);
    CHECK(f.command(Command::ClearFaults) == Result::Inhibited);
    f.feedback.stop = false;
    f.feedback.armed = false; f.feedback.idle = true;
    f.update(false);
    CHECK(f.command(Command::ClearFaults) == Result::Ok);
    CHECK(f.command(Command::Resume) == Result::InvalidState);
    CHECK(!f.engine.status().target.enable);
}
#endif // REHAB_ENABLE_TRAINING
#endif // REHAB_ENABLE_APPLICATION
}

int main(int argc, char** argv) {
    const bool long_run = argc > 1 && std::strcmp(argv[1], "--long") == 0;
    read_only_and_transport_guards();
#if REHAB_ENABLE_APPLICATION
    configuration_validation();
    calibration_and_staging();
    ownership_and_feature_switches();
#if REHAB_ENABLE_TRAINING
    complete_lifecycle_and_replay();
    observer_emergency_stop();
    duplicate_heartbeat_does_not_refresh();
    disconnected_lease_recovery();
    if (long_run) long_virtual_run();
#else
    (void)long_run;
#endif
#else
    (void)long_run;
    Fixture f;
    f.hello();
    CHECK(f.command(Command::Acquire) == Result::Unsupported);
    CHECK(f.command(Command::Enable) == Result::Unsupported);
    CHECK(f.command(Command::CommitConfig) == Result::Unsupported);
#endif
    std::cout << "Rehab service: " << checks << " checks passed; application=" << REHAB_ENABLE_APPLICATION
              << " training=" << REHAB_ENABLE_TRAINING << " statistics=" << REHAB_ENABLE_STATISTICS
              << " compensation=" << REHAB_ENABLE_COMPENSATION << '\n';
}
