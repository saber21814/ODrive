#pragma once

#include <cstddef>
#include <cstdint>

#ifndef REHAB_ENABLE_TRAINING
#define REHAB_ENABLE_TRAINING 1
#endif
#ifndef REHAB_ENABLE_STATISTICS
#define REHAB_ENABLE_STATISTICS 1
#endif
#ifndef REHAB_ENABLE_COMPENSATION
#define REHAB_ENABLE_COMPENSATION 1
#endif

// Platform-independent application control. One application task owns Engine;
// interrupt users consume a platform-protected copy of Status::target only.
namespace rehab {

enum class Result : uint16_t {
    Ok = 0, InvalidState, InvalidConfig, Busy, NotOwner, Faulted,
    Unsupported, Stale, BadSequence, QueueFull, Inhibited
};
enum class Mode : uint8_t {
    Isotonic = 0, Concentric, Eccentric, Isokinetic, Elasticity,
    WaterResistance, Fitness, Release
};
enum class State : uint8_t { Standby = 0, Ready, Running, Stopping, Paused, Fault };
enum ConfigFlag : uint32_t {
    ReturnEnabled = 1u, FrictionEnabled = 2u, InertiaEnabled = 4u,
    CompensationIdentified = 8u
};
enum Fault : uint32_t {
    FaultStop = 1u << 0, FaultNative = 1u << 1, FaultSensor = 1u << 2,
    FaultHeartbeat = 1u << 3, FaultLength = 1u << 4, FaultSpeed = 1u << 5,
    FaultForce = 1u << 6, FaultNumeric = 1u << 7,
    FaultStopTimeout = 1u << 8, FaultReturnTimeout = 1u << 9,
    FaultTargetExpired = 1u << 10, FaultPlatform = 1u << 11
};

struct Config {
    float radius_m = 0.0f;
    float motor_turns_per_drum_turn = 0.0f;
    float encoder_turns_per_motor_turn = 1.0f;
    float direction = 1.0f;
    float zero_turns = 0.0f;
    float efficiency = 0.0f;
    float force_scale_n_per_count = 0.0f;
    float force_offset_counts = 0.0f;
    float force_filter_hz = 3.0f;
    float min_length_m = 0.0f;
    float max_length_m = 0.0f;
    float max_speed_m_s = 0.0f;
    float max_force_n = 0.0f;
    float max_torque_nm = 0.0f;
    float torque_slew_nm_s = 0.0f;
    float force_kp = 0.0f;
    float force_ki = 0.0f;
    float integrator_limit_nm = 0.0f;
    float concentric_force_n = 0.0f;
    float eccentric_force_n = 0.0f;
    float speed_limit_m_s = 0.0f;
    float elastic_n_m = 0.0f;
    float water_n_s2_m2 = 0.0f;
    float phase_speed_hysteresis_m_s = 0.005f;
    float phase_min_distance_m = 0.005f;
    uint32_t phase_dwell_ms = 50;
    uint32_t heartbeat_timeout_ms = 250;
    uint32_t sensor_timeout_ms = 250;
    uint32_t target_timeout_ms = 20;
    uint32_t stop_timeout_ms = 2000;
    float return_speed_m_s = 0.0f;
    uint32_t return_timeout_ms = 5000;
    uint32_t fitness_duration_ms = 10000;
    uint32_t rfd_window_ms = 200;
    float friction_nm = 0.0f;
    float viscous_nm_s = 0.0f;
    float inertia_kg_m2 = 0.0f;
    float compensation_limit_nm = 0.0f;
    uint32_t flags = 0;
};

struct Feedback {
    uint32_t timestamp_us = 0;
    float position_turns = 0.0f, velocity_turns_s = 0.0f;
    float torque_nm = 0.0f, current_a = 0.0f, vbus_v = 0.0f;
    bool idle = true, armed = false, native_fault = false, stop = false;
};
struct ForceSample {
    uint32_t sequence = 0, timestamp_us = 0;
    int32_t raw = 0;
    bool valid = false;
};
struct Target {
    uint32_t sequence = 0, epoch = 0, created_us = 0;
    float torque_nm = 0.0f;
    bool enable = false;
};
struct Statistics {
    uint32_t repetitions = 0, force_samples = 0;
    float elapsed_s = 0.0f;
    float peak_force_n = 0.0f, mean_force_n = 0.0f;
    float peak_speed_m_s = 0.0f, mean_speed_m_s = 0.0f;
    float power_w = 0.0f, positive_energy_j = 0.0f, negative_energy_j = 0.0f;
    float rfd_n_s = 0.0f, peak_rfd_n_s = 0.0f;
    bool fitness_result_valid = false;
};
struct Status {
    State state = State::Standby;
    Mode mode = Mode::Isotonic;
    uint32_t faults = 0, session = 0, epoch = 1;
    bool config_valid = false, force_valid = false, rfd_valid = false;
    float length_m = 0.0f, speed_m_s = 0.0f, force_n = 0.0f;
    uint32_t sample_age_us = UINT32_MAX;
    Statistics stats{};
    Target target{};
};

class Engine {
public:
    Result configure(const Config& config);
    const Config& config() const { return config_; }
    const Status& status() const { return status_; }
    Result acquire(uint32_t session, const Feedback& feedback);
    Result release(uint32_t session, const Feedback& feedback);
    Result enable(uint32_t session);
    Result start(uint32_t session, Mode mode);
    Result pause(uint32_t session);
    Result resume(uint32_t session);
    Result stop(uint32_t session);
    Result clear_faults(uint32_t session);
    // Safety-task entry after native clear_errors, only without an application
    // lease. Fresh physical feedback is checked; configuration/calibration stay.
    Result clear_unowned_faults(const Feedback& feedback);
    Result heartbeat(uint32_t session, uint32_t now_us);
    Result tare(uint32_t session);
    Result calibrate(uint32_t session, float known_force_n);
    void update(uint32_t now_us, const Feedback& feedback, const ForceSample& force);
    void trip(uint32_t fault);
    void set_epoch(uint32_t epoch);
    void reset();
    static bool valid_config(const Config& config);

private:
    struct HistoryPoint { uint32_t us = 0; float force = 0.0f; };
    static constexpr size_t history_capacity = 96;
    static constexpr size_t calibration_capacity = 8;
    Config config_{};
    Status status_{};
    Feedback feedback_{};
    ForceSample force_{};
    HistoryPoint history_[history_capacity]{};
    int32_t calibration_raw_[calibration_capacity]{};
    size_t history_start_ = 0, history_size_ = 0, calibration_size_ = 0;
    size_t calibration_next_ = 0;
    uint32_t now_us_ = 0, last_update_us_ = 0, heartbeat_us_ = 0;
    uint32_t stop_started_us_ = 0, phase_started_us_ = 0, return_started_us_ = 0;
    uint32_t last_calibration_sequence_ = 0, tare_sequence_ = 0;
    float integral_nm_ = 0.0f, start_length_m_ = 0.0f;
    float phase_start_length_m_ = 0.0f, phase_candidate_length_m_ = 0.0f;
    float previous_motor_rad_s_ = 0.0f, tare_counts_ = 0.0f;
    // Keep accumulation precision independent of the float telemetry format.
    uint64_t statistics_elapsed_us_ = 0;
    double speed_integral_m_ = 0.0, force_mean_sum_ = 0.0;
    double positive_energy_j_ = 0.0, negative_energy_j_ = 0.0;
    float unfiltered_force_n_ = 0.0f;
    int phase_ = 0, phase_candidate_ = 0;
    bool initialized_ = false, have_force_ = false, filter_initialized_ = false;
    bool tare_valid_ = false, pause_after_stop_ = false;
    bool outward_seen_ = false, return_timing_ = false;
    bool new_force_ = false;
    float force_dt_s_ = 0.0f;

    Result owner(uint32_t session) const;
    bool stationary() const;
    bool inputs_ready() const;
    bool calibration_ready() const;
    void publish(float torque_nm, bool enable);
    void reset_training();
    void reset_force_history();
    void update_force(const ForceSample& force);
    void update_phase();
    void update_statistics(uint32_t delta_us);
    float desired_torque(float dt);
    float force_goal() const;
    Result begin_stopping(bool pause);
};

} // namespace rehab
