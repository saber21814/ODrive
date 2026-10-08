#include "../../Rehab/core/engine.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace rehab;
namespace {
unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; std::exit(1); } } while (0)
bool near(float a, float b, float tolerance = 0.0001f) { return std::fabs(a - b) <= tolerance; }
Config settings() {
    Config c;
    c.radius_m = 0.02f;
    c.motor_turns_per_drum_turn = 2;
    c.encoder_turns_per_motor_turn = 1;
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
    Config cfg = settings();
    Feedback feedback{};
    ForceSample sample{};
    uint32_t now = 0;
    bool send_heartbeat = true;
    bool sample_enabled = true;
    float force = 10;
    float length = 0.5f;
    float speed = 0;
    Fixture() { setup(cfg); }
    explicit Fixture(const Config& config) { setup(config); }
    void setup(const Config& config) {
        cfg = config;
        CHECK(engine.configure(config) == Result::Ok);
        CHECK(engine.acquire(42, feedback) == Result::Ok);
        update(true);
    }
    void update(bool fresh = false) {
        const float scale = cfg.direction * 6.28318530718f * cfg.radius_m /
                            cfg.motor_turns_per_drum_turn / cfg.encoder_turns_per_motor_turn;
        feedback.position_turns = length / scale + cfg.zero_turns;
        feedback.velocity_turns_s = speed / scale;
        feedback.timestamp_us = now;
        if (fresh && sample_enabled) {
            sample.sequence++;
            sample.timestamp_us = now;
            sample.raw = static_cast<int32_t>(force / cfg.force_scale_n_per_count + cfg.force_offset_counts);
            sample.valid = true;
        }
        if (send_heartbeat && now % 50000 == 0) CHECK(engine.heartbeat(42, now) == Result::Ok);
        engine.update(now, feedback, sample);
    }
    void advance(unsigned ms, bool follow_target = true) {
        for (unsigned i = 0; i < ms; ++i) {
            now += 1000;
            if (follow_target) {
                feedback.armed = engine.status().target.enable;
                feedback.idle = !feedback.armed;
            }
            length += speed * 0.001f;
            update(now % 13000 == 0);
        }
    }
    void run(Mode mode = Mode::Isotonic) {
        CHECK(engine.enable(42) == Result::Ok);
        CHECK(!engine.status().target.enable);
        CHECK(engine.start(42, mode) == Result::Ok);
    }
};

void configuration_and_ownership() {
    Engine empty;
    CHECK(!empty.status().config_valid);
    Feedback f;
    CHECK(empty.acquire(1, f) == Result::Ok);
    CHECK(empty.enable(1) == Result::InvalidConfig);
    CHECK(empty.configure(Config{}) == Result::InvalidConfig);
    CHECK(empty.configure(settings()) == Result::Ok);
    CHECK(empty.enable(1) == Result::Inhibited);
    Fixture x;
    Config invalid = x.cfg;
    invalid.radius_m = std::numeric_limits<float>::quiet_NaN();
    CHECK(x.engine.configure(invalid) == Result::InvalidConfig);
    CHECK(x.engine.config().radius_m == x.cfg.radius_m);
    invalid = x.cfg; invalid.flags = FrictionEnabled;
    CHECK(x.engine.configure(invalid) == Result::InvalidConfig);
    invalid = x.cfg; invalid.efficiency = 1.01f;
    CHECK(!Engine::valid_config(invalid));
    invalid = x.cfg; invalid.target_timeout_ms = 3000000;
    CHECK(!Engine::valid_config(invalid));
    invalid = x.cfg; invalid.flags = 1u << 31;
    CHECK(!Engine::valid_config(invalid));
    CHECK(x.engine.acquire(23, x.feedback) == Result::Busy);
    CHECK(x.engine.enable(23) == Result::NotOwner);
    CHECK(x.engine.heartbeat(23, 0) == Result::NotOwner);
    CHECK(x.engine.release(23, x.feedback) == Result::NotOwner);
    x.feedback.armed = true;
    CHECK(x.engine.release(42, x.feedback) == Result::Busy);
    x.feedback.armed = false;
    const uint32_t epoch = x.engine.status().epoch;
    CHECK(x.engine.release(42, x.feedback) == Result::Ok);
    CHECK(x.engine.status().session == 0 && x.engine.status().epoch != epoch);
    CHECK(x.engine.acquire(23, x.feedback) == Result::Ok);
    CHECK(x.engine.enable(42) == Result::NotOwner);
}
void conversion_and_direction() {
    for (float direction : {1.0f, -1.0f}) {
        auto c = settings(); c.direction = direction; c.encoder_turns_per_motor_turn = 0.5f; c.zero_turns = 7;
        Fixture x(c);
        CHECK(near(x.engine.status().length_m, 0.5f));
        x.speed = 0.1f;
        x.run();
        x.advance(200);
        CHECK(near(x.engine.status().speed_m_s, 0.1f));
        CHECK(near(x.engine.status().target.torque_nm, -direction * 0.25f));
        x.speed = -0.1f;
        x.advance(1);
        CHECK(x.engine.status().target.torque_nm == 0);
        CHECK(x.engine.status().stats.positive_energy_j > 0);
        CHECK(x.engine.status().stats.negative_energy_j > 0);
    }
}
void pause_stop_faults() {
    Fixture x;
    CHECK(x.engine.enable(42) == Result::Ok);
    CHECK(x.engine.start(42, Mode::Release) == Result::Unsupported);
    CHECK(x.engine.start(42, Mode::Isotonic) == Result::Ok);
    x.advance(200);
    CHECK(x.engine.configure(x.cfg) == Result::InvalidState);
    CHECK(x.engine.pause(42) == Result::Ok);
    const float before = x.engine.status().target.torque_nm;
    x.advance(1);
    CHECK(std::fabs(x.engine.status().target.torque_nm - before) <= 0.00201f);
    x.advance(150);
    CHECK(x.engine.status().state == State::Paused);
    CHECK(!x.engine.status().target.enable && x.engine.status().session == 42);
    CHECK(x.engine.start(42, Mode::Isotonic) == Result::InvalidState);
    CHECK(x.engine.resume(42) == Result::Ok);
    x.advance(200);
    CHECK(x.engine.stop(42) == Result::Ok);
    x.advance(150);
    CHECK(x.engine.status().state == State::Standby);
    CHECK(x.engine.status().session == 42);
    CHECK(x.engine.release(42, x.feedback) == Result::Ok);

    Fixture y;
    y.run(); y.advance(200);
    y.feedback.stop = true; y.advance(1);
    CHECK(y.engine.status().faults & FaultStop);
    CHECK(!y.engine.status().target.enable);
    CHECK(y.engine.clear_faults(42) == Result::Inhibited);
    y.feedback.stop = false; y.advance(1);
    CHECK(y.engine.clear_faults(42) == Result::Ok);
    CHECK(y.engine.status().state == State::Standby && !y.engine.status().target.enable);
    CHECK(y.engine.resume(42) == Result::InvalidState);
    CHECK(y.engine.start(42, Mode::Isotonic) == Result::InvalidState);

    Fixture z;
    z.run(); z.advance(200);
    CHECK(z.engine.pause(42) == Result::Ok);
    z.feedback.armed = true; z.feedback.idle = false;
    z.advance(2100, false);
    CHECK(z.engine.status().faults & FaultStopTimeout);
}
void watchdog_and_sensor_faults() {
    {
        Fixture x; x.run(); x.send_heartbeat = false; x.advance(251);
        CHECK(x.engine.status().faults & FaultHeartbeat);
        CHECK(!x.engine.status().target.enable);
    }
    {
        Fixture x; x.run(); x.sample_enabled = false; x.advance(251);
        CHECK(x.engine.status().faults & FaultSensor);
    }
    {
        Fixture x; x.run(); x.now += 25000; x.update(true);
        CHECK(x.engine.status().faults & FaultTargetExpired);
    }
    {
        Fixture x; x.run(); x.sample.raw = 8388607; ++x.sample.sequence; x.sample.timestamp_us = ++x.now;
        x.update();
        CHECK(x.engine.status().faults & FaultSensor);
    }
    {
        Fixture x; x.run(); x.sample.valid = false; x.advance(1);
        CHECK(x.engine.status().faults & FaultSensor);
    }
    {
        Fixture x; x.run(); x.force = 201; x.advance(13);
        CHECK(x.engine.status().faults & FaultForce);
    }
    {
        Fixture x; x.run(); x.force = -201; x.advance(13);
        CHECK(x.engine.status().faults & FaultForce);
    }
    {
        Fixture x; x.run(); x.length = 2.1f; x.advance(1);
        CHECK(x.engine.status().faults & FaultLength);
    }
    {
        Fixture x; x.run(); x.speed = 1.1f; x.advance(1);
        CHECK(x.engine.status().faults & FaultSpeed);
    }
    {
        Fixture x; x.run(); x.feedback.native_fault = true; x.advance(1);
        CHECK(x.engine.status().faults & FaultNative);
    }
    {
        Fixture x; x.run(); x.feedback.current_a = std::numeric_limits<float>::infinity(); x.advance(1);
        CHECK(x.engine.status().faults & FaultNumeric);
    }
    {
        Fixture x; x.run(); ++x.sample.sequence; x.sample.timestamp_us = x.now + 100000; x.advance(1);
        CHECK(x.engine.status().faults & FaultSensor);
    }
    {
        Fixture x; x.run(); x.sample_enabled = false;
        for (unsigned i = 0; i < 1000; ++i) {
            if (i % 100 == 0) {
                ++x.sample.sequence;
                x.sample.timestamp_us = x.now + 1000;
                x.sample.raw = 10000;
            }
            x.advance(1);
        }
        CHECK(x.engine.status().state == State::Running);
        CHECK(x.engine.status().force_valid);
    }
    {
        Fixture x; x.run(); x.now = 21000;
        CHECK(x.engine.heartbeat(42, x.now) == Result::Ok);
        x.engine.update(x.now, x.feedback, x.sample);
        CHECK(x.engine.status().faults & FaultTargetExpired);
    }
}
void unowned_fault_clear() {
    Fixture x;
    x.run(); x.advance(200);
    x.engine.trip(FaultPlatform);
    Feedback safe = x.feedback;
    safe.idle = true; safe.armed = false;
    CHECK(x.engine.clear_unowned_faults(safe) == Result::NotOwner);
    CHECK(x.engine.status().faults == FaultPlatform);
    CHECK(x.engine.release(42, safe) == Result::Ok);
    const auto epoch = x.engine.status().epoch;
    const auto prior_statistics = x.engine.status().stats;
    const Config prior_config = x.engine.config();
    const auto reject = [&](const Feedback& invalid) {
        CHECK(x.engine.clear_unowned_faults(invalid) == Result::Inhibited);
        CHECK(x.engine.status().faults == FaultPlatform);
        CHECK(x.engine.status().state == State::Fault);
        CHECK(x.engine.status().epoch == epoch);
        CHECK(!x.engine.status().target.enable && x.engine.status().target.torque_nm == 0);
    };
    Feedback invalid = safe; invalid.idle = false; reject(invalid);
    invalid = safe; invalid.armed = true; reject(invalid);
    invalid = safe; invalid.stop = true; reject(invalid);
    invalid = safe; invalid.native_fault = true; reject(invalid);
    float Feedback::* members[] = {
        &Feedback::position_turns, &Feedback::velocity_turns_s,
        &Feedback::torque_nm, &Feedback::current_a, &Feedback::vbus_v
    };
    for (auto member : members) {
        invalid = safe; invalid.*member = std::numeric_limits<float>::quiet_NaN(); reject(invalid);
        invalid = safe; invalid.*member = std::numeric_limits<float>::infinity(); reject(invalid);
    }
    CHECK(x.engine.clear_unowned_faults(safe) == Result::Ok);
    CHECK(x.engine.status().faults == 0 && x.engine.status().state == State::Standby);
    CHECK(x.engine.status().epoch == epoch + 1 && x.engine.status().session == 0);
    CHECK(!x.engine.status().target.enable && x.engine.status().target.torque_nm == 0);
    CHECK(x.engine.status().stats.force_samples == prior_statistics.force_samples);
    CHECK(x.engine.status().stats.elapsed_s == prior_statistics.elapsed_s);
    CHECK(x.engine.status().config_valid);
    CHECK(x.engine.config().radius_m == prior_config.radius_m);
    CHECK(x.engine.config().force_offset_counts == prior_config.force_offset_counts);
    CHECK(x.engine.config().force_scale_n_per_count == prior_config.force_scale_n_per_count);
    x.feedback = safe;
    x.advance(20, false);
    CHECK(x.engine.status().state == State::Standby && !x.engine.status().target.enable);
    CHECK(x.engine.acquire(42, safe) == Result::Ok);
    CHECK(x.engine.resume(42) == Result::InvalidState);
    CHECK(x.engine.start(42, Mode::Isotonic) == Result::InvalidState);
    CHECK(x.engine.enable(42) == Result::Ok);
    CHECK(!x.engine.status().target.enable);

    // An unowned STOP may occur before mechanical configuration exists. Clear
    // must preserve the tare calibration point and require explicit reacquisition.
    Engine unconfigured;
    Feedback feedback;
    CHECK(unconfigured.acquire(1, feedback) == Result::Ok);
    ForceSample force;
    for (uint32_t i = 1; i <= 8; ++i) {
        force = {i, i * 13000, 1000, true};
        feedback.timestamp_us = force.timestamp_us;
        unconfigured.update(force.timestamp_us, feedback, force);
    }
    CHECK(unconfigured.tare(1) == Result::Ok);
    CHECK(unconfigured.release(1, feedback) == Result::Ok);
    unconfigured.trip(FaultStop);
    CHECK(unconfigured.clear_unowned_faults(feedback) == Result::Ok);
    CHECK(!unconfigured.status().config_valid && unconfigured.config().force_offset_counts == 1000);
    CHECK(unconfigured.acquire(2, feedback) == Result::Ok);
    for (uint32_t i = 9; i <= 16; ++i) {
        force = {i, i * 13000, 11000, true};
        feedback.timestamp_us = force.timestamp_us;
        unconfigured.update(force.timestamp_us, feedback, force);
    }
    CHECK(unconfigured.calibrate(2, 10) == Result::Ok);
    CHECK(near(unconfigured.config().force_scale_n_per_count, 0.001f));
    CHECK(unconfigured.config().force_offset_counts == 1000);
    CHECK(unconfigured.enable(2) == Result::InvalidConfig);
}
void raw_calibration() {
    Engine e;
    Feedback feedback;
    CHECK(e.acquire(42, feedback) == Result::Ok);
    ForceSample f;
    for (unsigned i = 1; i <= 8; ++i) {
        f = {i, i * 13000, 1000, true}; feedback.timestamp_us = f.timestamp_us;
        e.update(f.timestamp_us, feedback, f);
    }
    CHECK(e.tare(42) == Result::Ok);
    CHECK(e.tare(42) == Result::Stale);
    CHECK(e.calibrate(42, 10) == Result::Stale);
    CHECK(!e.status().config_valid);
    for (unsigned i = 9; i <= 16; ++i) {
        f = {i, i * 13000, 11000, true}; feedback.timestamp_us = f.timestamp_us;
        e.update(f.timestamp_us, feedback, f);
    }
    CHECK(e.calibrate(42, 10) == Result::Ok);
    CHECK(near(e.config().force_scale_n_per_count, 0.001f));
    CHECK(e.config().force_offset_counts == 1000);
    CHECK(!e.status().config_valid);
    CHECK(e.enable(42) == Result::InvalidConfig);
    auto c = settings(); c.force_scale_n_per_count = e.config().force_scale_n_per_count;
    c.force_offset_counts = e.config().force_offset_counts;
    CHECK(e.configure(c) == Result::Ok);
    CHECK(!e.status().force_valid);
    f = {17, 221000, 11000, true}; feedback.timestamp_us = f.timestamp_us;
    CHECK(e.heartbeat(42, f.timestamp_us) == Result::Ok);
    e.update(f.timestamp_us, feedback, f);
    CHECK(near(e.status().force_n, 10));
    CHECK(e.enable(42) == Result::Ok);
    for (unsigned i = 18; i <= 25; ++i) {
        f = {i, i * 13000, 11000, true}; feedback.timestamp_us = f.timestamp_us;
        e.update(f.timestamp_us, feedback, f);
    }
    CHECK(e.tare(42) == Result::Ok);
    CHECK(e.status().state == State::Standby);
    CHECK(!e.status().force_valid);
    CHECK(e.enable(42) == Result::Inhibited);
    f = {26, 338000, 11000, true}; feedback.timestamp_us = f.timestamp_us;
    e.update(f.timestamp_us, feedback, f);
    CHECK(e.enable(42) == Result::Ok);
}
void new_sample_integral_and_modes() {
    auto c = settings(); c.force_ki = 0.1f; c.torque_slew_nm_s = 100;
    Fixture x(c); x.run(); x.advance(12);
    CHECK(near(x.engine.status().target.torque_nm, -0.25f));
    x.advance(1);
    CHECK(x.engine.status().target.torque_nm < -0.25f);
    const float integral_target = x.engine.status().target.torque_nm;
    x.advance(12);
    CHECK(near(x.engine.status().target.torque_nm, integral_target));
    x.advance(2000);
    CHECK(std::fabs(x.engine.status().target.torque_nm) <= 0.7501f);
    for (Mode mode : {Mode::Concentric, Mode::Eccentric, Mode::Isokinetic, Mode::Elasticity, Mode::WaterResistance}) {
        Fixture m;
        m.run(mode);
        m.speed = 0.5f;
        m.advance(500);
        CHECK(m.engine.status().state == State::Running);
        CHECK(m.engine.status().target.torque_nm < 0);
        CHECK(std::fabs(m.engine.status().target.torque_nm) <= m.cfg.max_torque_nm);
    }
}
void return_and_compensation() {
    auto c = settings(); c.flags = ReturnEnabled; c.return_timeout_ms = 100;
    Fixture x(c); x.run(); x.advance(150);
    x.speed = -0.25f; x.advance(1);
    CHECK(x.engine.status().target.torque_nm == 0);
    x.speed = -0.05f; x.advance(101);
    CHECK(x.engine.status().faults & FaultReturnTimeout);
    c.return_timeout_ms = 5000;
    Fixture y(c); y.run(); y.speed = -0.05f; y.advance(200);
    CHECK(y.engine.status().target.torque_nm < 0);
    CHECK(std::fabs(y.engine.status().target.torque_nm) <= 0.1251f);
    y.length = 0; y.speed = 0; y.advance(1);
    CHECK(y.engine.status().target.torque_nm == 0);
    Fixture stopping(c); stopping.run(); stopping.speed = -0.05f; stopping.advance(200);
    CHECK(stopping.engine.pause(42) == Result::Ok);
    stopping.speed = -0.25f; stopping.advance(1);
    CHECK(stopping.engine.status().target.torque_nm == 0);
    c.flags = FrictionEnabled | InertiaEnabled | CompensationIdentified;
    c.friction_nm = 0.05f; c.inertia_kg_m2 = 0.001f; c.compensation_limit_nm = 0.1f;
    Fixture z(c); z.run(); z.speed = 0.1f; z.advance(200);
    CHECK(near(z.engine.status().target.torque_nm, -0.20f));
}
void statistics_and_fitness() {
    Fixture x; x.run(); x.speed = 0.1f; x.advance(1000);
    CHECK(near(x.engine.status().stats.mean_force_n, 10));
    CHECK(near(x.engine.status().stats.mean_speed_m_s, 0.1f));
    CHECK(near(x.engine.status().stats.power_w, 1));
    CHECK(near(x.engine.status().stats.positive_energy_j, 1, 0.002f));
    x.speed = -0.1f; x.advance(1000);
    CHECK(near(x.engine.status().stats.negative_energy_j, 1, 0.002f));
    x.speed = 0.1f; x.advance(100);
    CHECK(x.engine.status().stats.repetitions == 1);
    CHECK(x.engine.status().rfd_valid);
    CHECK(near(x.engine.status().stats.rfd_n_s, 0));

    auto c = settings(); c.force_filter_hz = 40;
    Fixture ramp(c); ramp.run();
    for (unsigned i = 0; i < 1000; ++i) {
        ramp.force = 10 + static_cast<float>(i) * 0.01f;
        ramp.advance(1);
    }
    CHECK(ramp.engine.status().rfd_valid);
    CHECK(near(ramp.engine.status().stats.rfd_n_s, 10, 0.05f));
    c.fitness_duration_ms = 200;
    Fixture fitness(c); fitness.run(Mode::Fitness); fitness.advance(400);
    CHECK(fitness.engine.status().state == State::Standby);
    CHECK(fitness.engine.status().stats.fitness_result_valid);
    CHECK(!fitness.engine.status().target.enable);
}
void wrap_and_long_simulation() {
    Fixture x;
    x.now = UINT32_MAX - 9999;
    // Reset supplies a fresh ownership/clock epoch at the uint32 wrap boundary.
    x.engine.reset();
    CHECK(x.engine.configure(x.cfg) == Result::Ok);
    x.sample.sequence = 0;
    x.update(true);
    CHECK(x.engine.acquire(42, x.feedback) == Result::Ok);
    CHECK(x.engine.heartbeat(42, x.now) == Result::Ok);
    x.run();
    x.advance(40);
    CHECK(x.engine.status().state == State::Running);

    Fixture model;
    model.run();
    uint32_t rng = 0xC0DEC0DEu;
    for (unsigned i = 0; i < 120000; ++i) {
        rng = rng * 1664525u + 1013904223u;
        const float seconds = i * 0.001f;
        model.speed = 0.2f * std::cos(6.2831853f * seconds / 2.0f);
        model.force = 20.0f + static_cast<int>((rng >> 24) & 15u) * 0.01f;
        model.advance(1);
        CHECK(model.engine.status().state == State::Running);
        CHECK(std::isfinite(model.engine.status().target.torque_nm));
        CHECK(std::fabs(model.engine.status().target.torque_nm) <= model.cfg.max_torque_nm);
        if (model.speed < 0) CHECK(model.engine.status().target.torque_nm == 0);
    }
    CHECK(model.engine.status().stats.repetitions >= 58);
    CHECK(model.engine.status().stats.positive_energy_j > 100);
    CHECK(model.engine.status().stats.negative_energy_j > 100);
    std::cout << "Fixed-seed 120 s model: repetitions=" << model.engine.status().stats.repetitions
              << ", energies=" << model.engine.status().stats.positive_energy_j << "/"
              << model.engine.status().stats.negative_energy_j << " J\n";
}
}
int main() {
    configuration_and_ownership();
    conversion_and_direction();
    pause_stop_faults();
    watchdog_and_sensor_faults();
    unowned_fault_clear();
    raw_calibration();
    new_sample_integral_and_modes();
    return_and_compensation();
    statistics_and_fitness();
    wrap_and_long_simulation();
    std::cout << "Rehab core: " << checks << " checks passed; Engine bytes=" << sizeof(Engine) << "\n";
}
