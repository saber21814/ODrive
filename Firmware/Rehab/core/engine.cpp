#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rehab {
namespace {
constexpr float pi2 = 6.2831853071795864769f;
constexpr uint32_t half_clock = 0x80000000u;
constexpr int32_t raw_low = -8388608;
constexpr int32_t raw_high = 8388607;
bool finite(float value) { return std::isfinite(value); }
float clamp(float value, float low, float high) {
    return std::max(low, std::min(value, high));
}
bool time_limit(uint32_t ms) { return ms > 0 && ms <= 2000000; }
bool elapsed(uint32_t now, uint32_t then, uint32_t ms) {
    return now - then >= ms * 1000u;
}
bool forward(uint32_t value, uint32_t previous) {
    const uint32_t delta = value - previous;
    return delta != 0 && delta < half_clock;
}
float approach(float value, float target, float increment) {
    return value + clamp(target - value, -increment, increment);
}
}

bool Engine::valid_config(const Config& c) {
#if !REHAB_ENABLE_COMPENSATION
    if (c.flags & (FrictionEnabled | InertiaEnabled | CompensationIdentified)) return false;
#endif
    const float values[] = {
        c.radius_m, c.motor_turns_per_drum_turn, c.encoder_turns_per_motor_turn,
        c.direction, c.zero_turns, c.efficiency, c.force_scale_n_per_count,
        c.force_offset_counts, c.force_filter_hz, c.min_length_m, c.max_length_m,
        c.max_speed_m_s, c.max_force_n, c.max_torque_nm, c.torque_slew_nm_s,
        c.force_kp, c.force_ki, c.integrator_limit_nm, c.concentric_force_n,
        c.eccentric_force_n, c.speed_limit_m_s, c.elastic_n_m, c.water_n_s2_m2,
        c.phase_speed_hysteresis_m_s, c.phase_min_distance_m, c.return_speed_m_s,
        c.friction_nm, c.viscous_nm_s, c.inertia_kg_m2, c.compensation_limit_nm
    };
    for (float value : values) if (!finite(value)) return false;
    if (!(c.radius_m > 0 && c.motor_turns_per_drum_turn > 0 &&
          c.encoder_turns_per_motor_turn > 0 && (c.direction == 1 || c.direction == -1) &&
          c.efficiency > 0 && c.efficiency <= 1 && c.force_scale_n_per_count != 0 &&
          c.force_filter_hz > 0 && c.force_filter_hz <= 40 &&
          c.max_length_m > c.min_length_m && c.max_speed_m_s > 0 && c.max_force_n > 0 &&
          c.max_torque_nm > 0 && c.torque_slew_nm_s > 0 && c.force_kp >= 0 &&
          c.force_ki >= 0 && c.integrator_limit_nm >= 0 &&
          c.integrator_limit_nm <= c.max_torque_nm && c.concentric_force_n >= 0 &&
          c.concentric_force_n <= c.max_force_n && c.eccentric_force_n >= 0 &&
          c.eccentric_force_n <= c.max_force_n && c.speed_limit_m_s > 0 &&
          c.speed_limit_m_s <= c.max_speed_m_s && c.elastic_n_m >= 0 &&
          c.water_n_s2_m2 >= 0 && c.phase_speed_hysteresis_m_s > 0 &&
          c.phase_speed_hysteresis_m_s < c.max_speed_m_s && c.phase_min_distance_m > 0 &&
          c.phase_min_distance_m < c.max_length_m - c.min_length_m &&
          c.return_speed_m_s >= 0 && c.return_speed_m_s <= c.max_speed_m_s &&
          c.friction_nm >= 0 && c.viscous_nm_s >= 0 && c.inertia_kg_m2 >= 0 &&
          c.compensation_limit_nm >= 0 && c.compensation_limit_nm <= c.max_torque_nm)) return false;
    if (!time_limit(c.phase_dwell_ms) || !time_limit(c.heartbeat_timeout_ms) ||
        !time_limit(c.sensor_timeout_ms) || !time_limit(c.target_timeout_ms) ||
        !time_limit(c.stop_timeout_ms) || !time_limit(c.return_timeout_ms) ||
        !time_limit(c.fitness_duration_ms) || c.rfd_window_ms < 25 || c.rfd_window_ms > 1000 ||
        c.target_timeout_ms > c.heartbeat_timeout_ms ||
        c.target_timeout_ms > c.sensor_timeout_ms) return false;
    if (c.flags & ~(ReturnEnabled | FrictionEnabled | InertiaEnabled | CompensationIdentified)) return false;
    if ((c.flags & ReturnEnabled) && !(c.return_speed_m_s > 0)) return false;
    if ((c.flags & (FrictionEnabled | InertiaEnabled)) &&
        (!(c.flags & CompensationIdentified) || c.compensation_limit_nm <= 0)) return false;
    const float length_per_turn = pi2 * c.radius_m /
        c.motor_turns_per_drum_turn / c.encoder_turns_per_motor_turn;
    const float torque_per_n = c.radius_m / c.motor_turns_per_drum_turn / c.efficiency;
    return finite(length_per_turn) && length_per_turn > 0 && finite(torque_per_n) && torque_per_n > 0 &&
        finite(c.max_force_n * torque_per_n) && finite(c.max_torque_nm * c.motor_turns_per_drum_turn) &&
        finite(c.max_speed_m_s / c.radius_m * c.motor_turns_per_drum_turn) &&
        finite(c.force_scale_n_per_count * (16777216.0f + std::fabs(c.force_offset_counts)));
}

Result Engine::configure(const Config& config) {
    if (status_.state != State::Standby || !stationary()) return Result::InvalidState;
    if (!valid_config(config)) return Result::InvalidConfig;
    config_ = config;
    status_.config_valid = true;
    integral_nm_ = 0;
    reset_force_history();
    // A new conversion or filter must receive real samples before enable.
    have_force_ = false;
    calibration_size_ = 0;
    tare_valid_ = false;
    publish(0, false);
    return Result::Ok;
}

Result Engine::owner(uint32_t session) const {
    return session && session == status_.session ? Result::Ok : Result::NotOwner;
}
bool Engine::stationary() const { return feedback_.idle && !feedback_.armed; }
bool Engine::inputs_ready() const {
    return status_.config_valid && status_.force_valid && !status_.faults &&
        !feedback_.stop && !feedback_.native_fault &&
        !elapsed(now_us_, heartbeat_us_, config_.heartbeat_timeout_ms) &&
        status_.length_m >= config_.min_length_m && status_.length_m <= config_.max_length_m &&
        std::fabs(status_.speed_m_s) <= config_.max_speed_m_s &&
        status_.force_n <= config_.max_force_n &&
        std::fabs(unfiltered_force_n_) <= config_.max_force_n && initialized_;
}
Result Engine::acquire(uint32_t session, const Feedback& feedback) {
    if (!session) return Result::NotOwner;
    if (status_.session) return status_.session == session ? Result::Ok : Result::Busy;
    if (!feedback.idle || feedback.armed ||
        (status_.state != State::Standby && status_.state != State::Fault)) return Result::Busy;
    feedback_ = feedback;
    status_.session = session;
    heartbeat_us_ = now_us_;
    set_epoch(status_.epoch + 1u);
    return Result::Ok;
}
Result Engine::release(uint32_t session, const Feedback& feedback) {
    if (owner(session) != Result::Ok) return Result::NotOwner;
    if (!feedback.idle || feedback.armed ||
        (status_.state != State::Standby && status_.state != State::Fault)) return Result::Busy;
    feedback_ = feedback;
    status_.session = 0;
    set_epoch(status_.epoch + 1u);
    publish(0, false);
    return Result::Ok;
}
Result Engine::enable(uint32_t session) {
    if (owner(session) != Result::Ok) return Result::NotOwner;
    if (status_.faults) return Result::Faulted;
    if (status_.state != State::Standby || !stationary()) return Result::InvalidState;
    if (!status_.config_valid) return Result::InvalidConfig;
    if (!inputs_ready()) return Result::Inhibited;
    status_.state = State::Ready;
    publish(0, false);
    return Result::Ok;
}
void Engine::reset_training() {
    status_.stats = Statistics{};
    status_.rfd_valid = false;
    history_size_ = 0;
    history_start_ = 0;
    integral_nm_ = 0;
    speed_integral_m_ = 0;
    force_mean_sum_ = 0;
    statistics_elapsed_us_ = 0;
    positive_energy_j_ = negative_energy_j_ = 0;
    start_length_m_ = status_.length_m;
    phase_start_length_m_ = start_length_m_;
    phase_candidate_length_m_ = start_length_m_;
    phase_ = phase_candidate_ = 0;
    phase_started_us_ = now_us_;
    outward_seen_ = false;
    return_timing_ = false;
    previous_motor_rad_s_ = feedback_.velocity_turns_s / config_.encoder_turns_per_motor_turn * pi2;
}
Result Engine::start(uint32_t session, Mode mode) {
#if !REHAB_ENABLE_TRAINING
    (void)session; (void)mode;
    return Result::Unsupported;
#else
#if !REHAB_ENABLE_STATISTICS
    if (mode == Mode::Fitness) return Result::Unsupported;
#endif
    if (owner(session) != Result::Ok) return Result::NotOwner;
    if (mode == Mode::Release) return Result::Unsupported;
    if (static_cast<unsigned>(mode) > static_cast<unsigned>(Mode::Release)) return Result::InvalidConfig;
    if (status_.faults) return Result::Faulted;
    if (status_.state != State::Ready || !stationary()) return Result::InvalidState;
    if (!inputs_ready()) return Result::Inhibited;
    status_.mode = mode;
    reset_training();
    status_.state = State::Running;
    publish(0, true);
    return Result::Ok;
#endif
}
Result Engine::begin_stopping(bool pause) {
    pause_after_stop_ = pause;
    stop_started_us_ = now_us_;
    integral_nm_ = 0;
    status_.state = State::Stopping;
    return Result::Ok;
}
Result Engine::pause(uint32_t session) {
    if (owner(session) != Result::Ok) return Result::NotOwner;
    if (status_.faults) return Result::Faulted;
    if (status_.state != State::Running) return Result::InvalidState;
    return begin_stopping(true);
}
Result Engine::resume(uint32_t session) {
#if !REHAB_ENABLE_TRAINING
    (void)session;
    return Result::Unsupported;
#else
    if (owner(session) != Result::Ok) return Result::NotOwner;
    if (status_.faults) return Result::Faulted;
    if (status_.state != State::Paused || !stationary()) return Result::InvalidState;
    if (!inputs_ready()) return Result::Inhibited;
    integral_nm_ = 0;
    phase_ = phase_candidate_ = 0;
    phase_start_length_m_ = phase_candidate_length_m_ = status_.length_m;
    phase_started_us_ = now_us_;
    outward_seen_ = false;
    return_timing_ = false;
    history_size_ = 0;
    status_.rfd_valid = false;
    previous_motor_rad_s_ = feedback_.velocity_turns_s / config_.encoder_turns_per_motor_turn * pi2;
    status_.state = State::Running;
    publish(0, true);
    return Result::Ok;
#endif
}
Result Engine::stop(uint32_t session) {
    if (owner(session) != Result::Ok) return Result::NotOwner;
    if (status_.faults) return Result::Faulted;
    if (status_.state == State::Stopping) { pause_after_stop_ = false; return Result::Ok; }
    if (status_.state == State::Standby) return Result::Ok;
    return begin_stopping(false);
}
Result Engine::clear_faults(uint32_t session) {
    if (owner(session) != Result::Ok) return Result::NotOwner;
    if (!stationary() || feedback_.stop || feedback_.native_fault) return Result::Inhibited;
    if (status_.state != State::Fault) return Result::InvalidState;
    status_.faults = 0;
    status_.state = State::Standby;
    integral_nm_ = 0;
    set_epoch(status_.epoch + 1u);
    publish(0, false);
    return Result::Ok;
}
Result Engine::clear_unowned_faults(const Feedback& feedback) {
    if (status_.session != 0) return Result::NotOwner;
    if (!feedback.idle || feedback.armed || feedback.stop || feedback.native_fault ||
        !finite(feedback.position_turns) || !finite(feedback.velocity_turns_s) ||
        !finite(feedback.torque_nm) || !finite(feedback.current_a) || !finite(feedback.vbus_v))
        return Result::Inhibited;
    feedback_ = feedback;
    status_.faults = 0;
    status_.state = State::Standby;
    integral_nm_ = 0;
    start_length_m_ = phase_start_length_m_ = phase_candidate_length_m_ = 0;
    previous_motor_rad_s_ = 0;
    phase_ = phase_candidate_ = 0;
    pause_after_stop_ = outward_seen_ = return_timing_ = false;
    stop_started_us_ = phase_started_us_ = return_started_us_ = now_us_;
    new_force_ = false;
    force_dt_s_ = 0;
    history_start_ = history_size_ = 0;
    status_.rfd_valid = false;
    status_.stats.power_w = status_.stats.rfd_n_s = 0;
    status_.stats.fitness_result_valid = false;
    // Keep completed statistics for diagnosis and retain both calibration points
    // and raw-sample stability history. A subsequent start resets training data.
    set_epoch(status_.epoch + 1u);
    publish(0, false);
    return Result::Ok;
}
Result Engine::heartbeat(uint32_t session, uint32_t now_us) {
    if (owner(session) != Result::Ok) return Result::NotOwner;
    if (now_us - now_us_ >= half_clock) return Result::Stale;
    heartbeat_us_ = now_us;
    return Result::Ok;
}
void Engine::set_epoch(uint32_t epoch) {
    status_.epoch = epoch ? epoch : 1;
    status_.target.epoch = status_.epoch;
    status_.target.enable = false;
    status_.target.torque_nm = 0;
    ++status_.target.sequence;
    status_.target.created_us = now_us_;
}
void Engine::publish(float torque_nm, bool enable) {
    status_.target.torque_nm = enable ? torque_nm : 0;
    status_.target.enable = enable;
    status_.target.epoch = status_.epoch;
    status_.target.created_us = now_us_;
    ++status_.target.sequence;
}
void Engine::trip(uint32_t fault) {
    status_.faults |= fault ? fault : static_cast<uint32_t>(FaultPlatform);
    status_.state = State::Fault;
    integral_nm_ = 0;
    status_.stats.fitness_result_valid = false;
    publish(0, false);
}
void Engine::reset() { *this = Engine{}; }

void Engine::reset_force_history() {
    history_start_ = history_size_ = 0;
    filter_initialized_ = false;
    status_.force_valid = false;
    status_.rfd_valid = false;
    status_.sample_age_us = UINT32_MAX;
}
bool Engine::calibration_ready() const {
    if (!stationary() || !have_force_ || !force_.valid || calibration_size_ < calibration_capacity ||
        (status_.state != State::Standby && status_.state != State::Ready && status_.state != State::Paused) ||
        elapsed(now_us_, force_.timestamp_us, config_.sensor_timeout_ms) ||
        (last_calibration_sequence_ != 0 && !forward(force_.sequence, last_calibration_sequence_))) return false;
    int32_t low = calibration_raw_[0], high = low;
    for (size_t i = 1; i < calibration_size_; ++i) {
        low = std::min(low, calibration_raw_[i]); high = std::max(high, calibration_raw_[i]);
    }
    const float threshold = config_.force_scale_n_per_count == 0 ? 32.0f :
        clamp(0.25f / std::fabs(config_.force_scale_n_per_count), 32.0f, 1024.0f);
    return static_cast<float>(high - low) <= threshold;
}
Result Engine::tare(uint32_t session) {
    if (owner(session) != Result::Ok) return Result::NotOwner;
    if (!stationary() || (status_.state != State::Standby && status_.state != State::Ready &&
                        status_.state != State::Paused)) return Result::InvalidState;
    if (!calibration_ready()) return Result::Stale;
    double sum = 0;
    for (size_t i = 0; i < calibration_size_; ++i) sum += calibration_raw_[i];
    tare_counts_ = static_cast<float>(sum / calibration_size_);
    config_.force_offset_counts = tare_counts_;
    tare_valid_ = true;
    tare_sequence_ = force_.sequence;
    last_calibration_sequence_ = force_.sequence;
    calibration_size_ = 0;
    status_.config_valid = valid_config(config_);
    reset_force_history();
    if (status_.state == State::Ready) status_.state = State::Standby;
    return Result::Ok;
}
Result Engine::calibrate(uint32_t session, float known_force_n) {
    if (owner(session) != Result::Ok) return Result::NotOwner;
    if (!finite(known_force_n) || known_force_n <= 0) return Result::InvalidConfig;
    if (!stationary() || (status_.state != State::Standby && status_.state != State::Ready &&
                        status_.state != State::Paused)) return Result::InvalidState;
    if (!tare_valid_) return Result::InvalidState;
    if (!calibration_ready() || !forward(force_.sequence, tare_sequence_)) return Result::Stale;
    double sum = 0;
    for (size_t i = 0; i < calibration_size_; ++i) sum += calibration_raw_[i];
    const float delta = static_cast<float>(sum / calibration_size_) - tare_counts_;
    int32_t low = calibration_raw_[0], high = low;
    for (size_t i = 1; i < calibration_size_; ++i) {
        low = std::min(low, calibration_raw_[i]); high = std::max(high, calibration_raw_[i]);
    }
    if (std::fabs(delta) <= std::max(32.0f, 4.0f * static_cast<float>(high - low))) return Result::InvalidConfig;
    const float scale = known_force_n / delta;
    if (!finite(scale) || scale == 0 || !finite(scale * 16777216.0f)) return Result::InvalidConfig;
    config_.force_scale_n_per_count = scale;
    config_.force_offset_counts = tare_counts_;
    status_.config_valid = valid_config(config_);
    last_calibration_sequence_ = force_.sequence;
    calibration_size_ = 0;
    reset_force_history();
    if (status_.state == State::Ready) status_.state = State::Standby;
    return Result::Ok;
}

void Engine::update_force(const ForceSample& input) {
    new_force_ = false;
    force_dt_s_ = 0;
    const bool changed = !have_force_ || forward(input.sequence, force_.sequence);
    if (changed) {
        const bool time_forward = !have_force_ || forward(input.timestamp_us, force_.timestamp_us);
        const uint32_t delta = have_force_ ? input.timestamp_us - force_.timestamp_us : 0;
        const bool valid = input.valid && input.raw > raw_low && input.raw < raw_high &&
            time_forward && now_us_ - input.timestamp_us < half_clock;
        force_ = input;
        force_.valid = valid;
        have_force_ = true;
        if (valid) {
            calibration_raw_[calibration_next_] = input.raw;
            calibration_next_ = (calibration_next_ + 1) % calibration_capacity;
            calibration_size_ = std::min(calibration_size_ + 1, calibration_capacity);
            const float converted = (static_cast<float>(input.raw) - config_.force_offset_counts) *
                                    config_.force_scale_n_per_count;
            if (config_.force_scale_n_per_count != 0 && finite(converted)) {
                unfiltered_force_n_ = converted;
                const float force = std::max(0.0f, converted);
                force_dt_s_ = delta * 1e-6f;
                if (!filter_initialized_ || delta >= config_.sensor_timeout_ms * 1000u) {
                    status_.force_n = force;
                    filter_initialized_ = true;
                    force_dt_s_ = 0;
                    history_size_ = 0;
                } else {
                    const float alpha = (pi2 * config_.force_filter_hz * force_dt_s_) /
                                        (1 + pi2 * config_.force_filter_hz * force_dt_s_);
                    status_.force_n += alpha * (force - status_.force_n);
                }
                new_force_ = true;
                // Peak protection uses the unfiltered sample; filtering cannot hide an overload.
                if (status_.state == State::Running || status_.state == State::Stopping || status_.state == State::Ready) {
                    if (std::fabs(converted) > config_.max_force_n) trip(FaultForce);
                }
#if REHAB_ENABLE_STATISTICS
                if (status_.state == State::Running) {
                    if (history_size_ == history_capacity) {
                        history_start_ = (history_start_ + 1) % history_capacity;
                        --history_size_;
                    }
                    history_[(history_start_ + history_size_) % history_capacity] = {input.timestamp_us, status_.force_n};
                    ++history_size_;
                    const uint32_t window = config_.rfd_window_ms * 1000u;
                    while (history_size_ > 2 &&
                           input.timestamp_us - history_[(history_start_ + 1) % history_capacity].us >= window) {
                        history_start_ = (history_start_ + 1) % history_capacity;
                        --history_size_;
                    }
                    status_.rfd_valid = history_size_ >= 2 &&
                        input.timestamp_us - history_[history_start_].us >= window;
                    if (status_.rfd_valid) {
                        const auto& old = history_[history_start_];
                        const auto& next = history_[(history_start_ + 1) % history_capacity];
                        const uint32_t fraction_us = input.timestamp_us - old.us - window;
                        const float old_force = old.force + (next.force - old.force) *
                            static_cast<float>(fraction_us) / static_cast<float>(next.us - old.us);
                        status_.stats.rfd_n_s = (status_.force_n - old_force) / (window * 1e-6f);
                        status_.stats.peak_rfd_n_s = std::max(status_.stats.peak_rfd_n_s, status_.stats.rfd_n_s);
                    }
                }
#endif
            }
        } else {
            calibration_size_ = 0;
            reset_force_history();
        }
    } else if (!input.valid) {
        // Explicit driver invalidation is authoritative even without a new sequence.
        force_.valid = false;
        calibration_size_ = 0;
        reset_force_history();
    }
    status_.sample_age_us = have_force_ ? now_us_ - force_.timestamp_us : UINT32_MAX;
    status_.force_valid = have_force_ && force_.valid && filter_initialized_ &&
        config_.force_scale_n_per_count != 0 &&
        status_.sample_age_us < config_.sensor_timeout_ms * 1000u;
    if (!status_.force_valid) status_.rfd_valid = false;
}

void Engine::update_phase() {
    const int candidate = status_.speed_m_s > config_.phase_speed_hysteresis_m_s ? 1 :
                          (status_.speed_m_s < -config_.phase_speed_hysteresis_m_s ? -1 : 0);
    if (candidate == 0 || candidate == phase_) {
        phase_candidate_ = 0;
        return;
    }
    if (candidate != phase_candidate_) {
        phase_candidate_ = candidate;
        phase_started_us_ = now_us_;
        phase_candidate_length_m_ = status_.length_m;
        return;
    }
    if (!elapsed(now_us_, phase_started_us_, config_.phase_dwell_ms) ||
        candidate * (status_.length_m - phase_candidate_length_m_) < config_.phase_min_distance_m) return;
    const int previous = phase_;
    const float previous_distance = status_.length_m - phase_start_length_m_;
    phase_ = candidate;
    phase_candidate_ = 0;
    phase_start_length_m_ = status_.length_m;
    if (previous == 1 && candidate == -1 && previous_distance >= config_.phase_min_distance_m)
        outward_seen_ = true;
    if (previous == -1 && candidate == 1 && outward_seen_ &&
        -previous_distance >= config_.phase_min_distance_m) {
#if REHAB_ENABLE_STATISTICS
        ++status_.stats.repetitions;
#endif
        outward_seen_ = false;
    }
}
float Engine::force_goal() const {
    switch (status_.mode) {
        case Mode::Concentric: return phase_ < 0 ? config_.eccentric_force_n : config_.concentric_force_n;
        case Mode::Eccentric: return phase_ > 0 ? config_.concentric_force_n : config_.eccentric_force_n;
        case Mode::Isokinetic: {
            const float overspeed = std::max(0.0f, status_.speed_m_s - config_.speed_limit_m_s);
            return config_.concentric_force_n + config_.max_force_n * overspeed / config_.speed_limit_m_s;
        }
        case Mode::Elasticity: return config_.elastic_n_m * std::max(0.0f, status_.length_m - start_length_m_);
        case Mode::WaterResistance: return config_.water_n_s2_m2 * status_.speed_m_s * status_.speed_m_s;
        case Mode::Isotonic: case Mode::Fitness: return config_.concentric_force_n;
        case Mode::Release: return 0;
    }
    return 0;
}
float Engine::desired_torque(float dt) {
    // Encoder direction defines outward motion; positive tension always pulls inward.
    float desired_force = clamp(force_goal(), 0, config_.max_force_n);
    const float inward_speed = -status_.speed_m_s;
    if (!(config_.flags & ReturnEnabled) && inward_speed > 0) {
        integral_nm_ = 0;
        return_timing_ = false;
        return 0;
    }
    if (config_.flags & ReturnEnabled) {
        if (inward_speed > config_.phase_speed_hysteresis_m_s)
            desired_force = config_.eccentric_force_n;
        if (inward_speed > config_.phase_speed_hysteresis_m_s) {
            if (!return_timing_) { return_timing_ = true; return_started_us_ = now_us_; }
            if (elapsed(now_us_, return_started_us_, config_.return_timeout_ms)) {
                trip(FaultReturnTimeout); return 0;
            }
            // Taper commanded tension from half the allowed return speed to zero
            // at the speed limit. Never increase pulling torque to brake inward motion.
            desired_force *= clamp(2.0f * (1.0f - inward_speed / config_.return_speed_m_s), 0, 1);
        } else if (status_.speed_m_s > config_.phase_speed_hysteresis_m_s) {
            return_timing_ = false;
        }
        const float remaining = status_.length_m - config_.min_length_m;
        desired_force *= clamp(remaining / config_.phase_min_distance_m, 0, 1);
        if (desired_force == 0) integral_nm_ = 0;
    }
    const float feedforward = desired_force * config_.radius_m /
                              config_.motor_turns_per_drum_turn / config_.efficiency;
    const float error = desired_force - status_.force_n;
    const float proportional = config_.force_kp * error;
    if (new_force_ && force_dt_s_ > 0) {
        const float increment = config_.force_ki * error * force_dt_s_;
        const float raw = feedforward + proportional + integral_nm_;
        if ((raw < config_.max_torque_nm || increment < 0) && (raw > 0 || increment > 0))
            integral_nm_ = clamp(integral_nm_ + increment, -config_.integrator_limit_nm, config_.integrator_limit_nm);
    }
    float tension = clamp(feedforward + proportional + integral_nm_, 0, config_.max_torque_nm);
#if REHAB_ENABLE_COMPENSATION
    const float motor_rad_s = feedback_.velocity_turns_s / config_.encoder_turns_per_motor_turn * pi2;
    const float outward_rad_s = motor_rad_s * config_.direction;
    float compensation = 0;
    if (config_.flags & FrictionEnabled)
        compensation += config_.friction_nm + config_.viscous_nm_s * std::fabs(motor_rad_s);
    if ((config_.flags & InertiaEnabled) && dt > 0)
        compensation += config_.inertia_kg_m2 *
                        (motor_rad_s - previous_motor_rad_s_) * config_.direction / dt;
    previous_motor_rad_s_ = motor_rad_s;
    // Compensating passive losses reduces resisting motor torque. Identification
    // terms are bounded separately and cannot reverse the tension direction.
    if (outward_rad_s > 0) tension -= clamp(compensation, -config_.compensation_limit_nm, config_.compensation_limit_nm);
#else
    (void)dt;
#endif
    tension = clamp(tension, 0, config_.max_torque_nm);
    if ((config_.flags & ReturnEnabled) &&
        (inward_speed >= config_.return_speed_m_s || status_.length_m <= config_.min_length_m)) return 0;
    return -config_.direction * tension;
}
void Engine::update_statistics(uint32_t delta_us) {
#if REHAB_ENABLE_STATISTICS
    auto& s = status_.stats;
    statistics_elapsed_us_ += delta_us;
    const double dt = static_cast<double>(delta_us) * 1e-6;
    const double elapsed_s = static_cast<double>(statistics_elapsed_us_) * 1e-6;
    s.elapsed_s = static_cast<float>(elapsed_s);
    if (new_force_) {
        ++s.force_samples;
        force_mean_sum_ += static_cast<double>(status_.force_n);
        s.mean_force_n = static_cast<float>(force_mean_sum_ / s.force_samples);
        s.peak_force_n = std::max(s.peak_force_n, status_.force_n);
    }
    const float speed = std::fabs(status_.speed_m_s);
    speed_integral_m_ += static_cast<double>(speed) * dt;
    s.mean_speed_m_s = elapsed_s > 0 ? static_cast<float>(speed_integral_m_ / elapsed_s) : 0;
    s.peak_speed_m_s = std::max(s.peak_speed_m_s, speed);
    const double power = static_cast<double>(status_.force_n) * static_cast<double>(status_.speed_m_s);
    s.power_w = static_cast<float>(power);
    if (power >= 0) positive_energy_j_ += power * dt;
    else negative_energy_j_ -= power * dt;
    s.positive_energy_j = static_cast<float>(positive_energy_j_);
    s.negative_energy_j = static_cast<float>(negative_energy_j_);
#else
    (void)delta_us;
    status_.rfd_valid = false;
#endif
}

void Engine::update(uint32_t now_us, const Feedback& feedback, const ForceSample& force) {
    const uint32_t delta_us = initialized_ ? now_us - last_update_us_ : 0;
    now_us_ = now_us;
    last_update_us_ = now_us;
    const bool was_initialized = initialized_;
    initialized_ = true;
    feedback_ = feedback;
    if (feedback.stop) trip(FaultStop);
    if (feedback.native_fault) trip(FaultNative);
    const bool feedback_valid = finite(feedback.position_turns) && finite(feedback.velocity_turns_s) &&
        finite(feedback.torque_nm) && finite(feedback.current_a) && finite(feedback.vbus_v);
    if (!feedback_valid) trip(FaultNumeric);
    if (status_.config_valid && feedback_valid) {
        const float multiplier = config_.direction * pi2 * config_.radius_m /
            config_.motor_turns_per_drum_turn / config_.encoder_turns_per_motor_turn;
        status_.length_m = (feedback.position_turns - config_.zero_turns) * multiplier;
        status_.speed_m_s = feedback.velocity_turns_s * multiplier;
        if (!finite(status_.length_m) || !finite(status_.speed_m_s)) trip(FaultNumeric);
    }
    update_force(force);
    const bool monitoring = status_.state == State::Ready || status_.state == State::Running ||
                            status_.state == State::Stopping;
    if (monitoring) {
        if (!status_.force_valid) trip(FaultSensor);
        if (elapsed(now_us_, heartbeat_us_, config_.heartbeat_timeout_ms)) trip(FaultHeartbeat);
        if (status_.length_m < config_.min_length_m || status_.length_m > config_.max_length_m) trip(FaultLength);
        if (std::fabs(status_.speed_m_s) > config_.max_speed_m_s) trip(FaultSpeed);
        if (status_.force_n > config_.max_force_n ||
            std::fabs(unfiltered_force_n_) > config_.max_force_n) trip(FaultForce);
        if (now_us_ - feedback.timestamp_us >= config_.target_timeout_ms * 1000u ||
            (was_initialized && delta_us >= config_.target_timeout_ms * 1000u)) trip(FaultTargetExpired);
    }
    const float dt = delta_us < half_clock ? delta_us * 1e-6f : 0;
    if (status_.state == State::Running) {
        update_phase();
        update_statistics(delta_us);
        if (status_.mode == Mode::Fitness && status_.stats.elapsed_s >= config_.fitness_duration_ms * 0.001f) {
            status_.stats.fitness_result_valid = true;
            begin_stopping(false);
        } else {
            const float requested = desired_torque(dt);
            if (status_.state == State::Fault) return;
            if (!finite(requested)) { trip(FaultNumeric); return; }
            float torque = approach(status_.target.torque_nm, requested, config_.torque_slew_nm_s * dt);
            // A no-active-return interlock overrides slew limiting: residual inward
            // torque would otherwise inject power while the user returns the cable.
            if ((!(config_.flags & ReturnEnabled) && status_.speed_m_s < 0) ||
                ((config_.flags & ReturnEnabled) &&
                 (-status_.speed_m_s >= config_.return_speed_m_s || status_.length_m <= config_.min_length_m))) torque = 0;
            publish(clamp(torque, -config_.max_torque_nm, config_.max_torque_nm), true);
        }
    }
    if (status_.state == State::Stopping) {
        float torque = approach(status_.target.torque_nm, 0, config_.torque_slew_nm_s * dt);
        if (!(config_.flags & ReturnEnabled) && status_.speed_m_s < 0) torque = 0;
        if (config_.flags & ReturnEnabled) {
            if (-status_.speed_m_s >= config_.return_speed_m_s || status_.length_m <= config_.min_length_m) torque = 0;
            if (return_timing_ && elapsed(now_us_, return_started_us_, config_.return_timeout_ms)) {
                trip(FaultReturnTimeout);
                return;
            }
        }
        publish(torque, torque != 0);
        if (torque == 0 && stationary()) {
            status_.state = pause_after_stop_ ? State::Paused : State::Standby;
        } else if (elapsed(now_us_, stop_started_us_, config_.stop_timeout_ms)) {
            trip(FaultStopTimeout);
        }
    } else if (status_.state != State::Running) {
        publish(0, false);
    }
}

} // namespace rehab
