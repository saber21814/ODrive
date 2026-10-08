// Deterministic software-only mass/load model using the production Engine.
// This model verifies state/control integration; it is not a motor identification.
#include "../../Rehab/core/engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>

int main(int argc, char** argv) {
    const unsigned seconds = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 60;
    if (seconds < 1 || seconds > 3600) {
        std::cerr << "Usage: rehab_sim [seconds:1..3600]\n";
        return 2;
    }
    rehab::Config c;
    c.radius_m = 0.025f; c.motor_turns_per_drum_turn = 3;
    c.efficiency = 0.9f; c.force_scale_n_per_count = 0.001f;
    c.max_length_m = 2; c.max_speed_m_s = 2; c.max_force_n = 200;
    c.max_torque_nm = 2; c.torque_slew_nm_s = 2;
    c.concentric_force_n = 20; c.eccentric_force_n = 8;
    c.speed_limit_m_s = 0.5f; c.return_speed_m_s = 0.2f;
    c.force_kp = 0.0005f; c.force_ki = 0.001f; c.integrator_limit_nm = 0.1f;
    c.phase_min_distance_m = 0.002f; c.phase_dwell_ms = 20;
    rehab::Engine engine;
    if (engine.configure(c) != rehab::Result::Ok) return 3;
    rehab::Feedback feedback;
    rehab::ForceSample force{1, 0, 0, true};
    const float encoder_turns_per_m = c.motor_turns_per_drum_turn / (6.28318530718f * c.radius_m);
    float length = 0.6f, speed = 0, actual_torque = 0;
    feedback.position_turns = length * encoder_turns_per_m;
    engine.update(0, feedback, force);
    if (engine.acquire(1, feedback) != rehab::Result::Ok || engine.enable(1) != rehab::Result::Ok ||
        engine.start(1, rehab::Mode::Isotonic) != rehab::Result::Ok) return 4;
    std::cout << "time_s,length_m,speed_m_s,force_n,command_nm,actual_nm,state,faults\n";
    uint32_t rng = 0x52454841u;
    uint32_t samples = 0;
    for (uint32_t ms = 1; ms <= seconds * 1000u + 1000u; ++ms) {
        const uint32_t now = ms * 1000u;
        const float time_s = ms * 0.001f;
        const auto target = engine.status().target;
        const float command = target.enable ? target.torque_nm : 0;
        actual_torque += (command - actual_torque) * 0.05f; // 20 ms actuator lag.
        const float tension = std::max(0.0f, -actual_torque * c.motor_turns_per_drum_turn * c.efficiency / c.radius_m);
        // 4 kg effective moving load, 18 N*s/m viscous loss, passive 80 N/m
        // restoring load and a user's periodic outward force. The passive load
        // returns the cable; ReturnEnabled remains disabled in this run.
        const float user_force = 20.0f + 15.0f * std::sin(6.28318530718f * time_s / 3.0f);
        const float acceleration = (user_force - tension - 18 * speed - 80 * (length - 0.6f)) / 4;
        speed += acceleration * 0.001f;
        length += speed * 0.001f;
        feedback.timestamp_us = now;
        feedback.position_turns = length * encoder_turns_per_m;
        feedback.velocity_turns_s = speed * encoder_turns_per_m;
        feedback.torque_nm = actual_torque;
        feedback.current_a = actual_torque / 0.1f;
        feedback.vbus_v = 24;
        feedback.armed = target.enable;
        feedback.idle = !target.enable;
        if (ms % 13 == 0) {
            rng = rng * 1664525u + 1013904223u;
            const float noise = (static_cast<float>((rng >> 24) & 255u) - 127.5f) * 0.0005f;
            force = {++samples + 1, now, static_cast<int32_t>((tension + noise) / c.force_scale_n_per_count), true};
        }
        if (ms % 50 == 0 && engine.heartbeat(1, now) != rehab::Result::Ok) return 5;
        if (ms == seconds * 1000u && engine.stop(1) != rehab::Result::Ok) return 6;
        engine.update(now, feedback, force);
        if (ms % 100 == 0) {
            const auto& s = engine.status();
            std::cout << std::fixed << std::setprecision(5) << time_s << ',' << s.length_m << ','
                      << s.speed_m_s << ',' << s.force_n << ',' << s.target.torque_nm << ','
                      << actual_torque << ',' << static_cast<unsigned>(s.state) << ',' << s.faults << '\n';
        }
        if (engine.status().state == rehab::State::Fault) {
            std::cerr << "Simulation fault " << engine.status().faults << " at " << time_s << " s\n";
            return 7;
        }
    }
    const auto& s = engine.status();
    if (s.state != rehab::State::Standby || s.target.enable) return 8;
    std::cerr << "Simulation passed; " << seconds << " s + controlled stop; repetitions=" << s.stats.repetitions
              << "; energy J=" << s.stats.positive_energy_j << '/' << s.stats.negative_energy_j << '\n';
    return 0;
}
