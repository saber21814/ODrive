#include "../../Rehab/core/engine.hpp"
#include <iostream>

int main() {
    rehab::Engine engine;
    rehab::Config config;
#if !REHAB_ENABLE_TRAINING
    if (engine.start(0, rehab::Mode::Isotonic) != rehab::Result::Unsupported ||
        engine.resume(0) != rehab::Result::Unsupported) return 1;
#elif !REHAB_ENABLE_STATISTICS
    if (engine.start(0, rehab::Mode::Fitness) != rehab::Result::Unsupported) return 2;
#endif
#if !REHAB_ENABLE_COMPENSATION
    config.flags = rehab::CompensationIdentified | rehab::FrictionEnabled;
    if (engine.configure(config) != rehab::Result::InvalidConfig) return 3;
#endif
    engine.update(1000, rehab::Feedback{}, rehab::ForceSample{});
    if (engine.status().rfd_valid || engine.status().stats.force_samples) return 4;
    std::cout << "Feature switches passed: training=" << REHAB_ENABLE_TRAINING
              << " statistics=" << REHAB_ENABLE_STATISTICS
              << " compensation=" << REHAB_ENABLE_COMPENSATION << '\n';
    return 0;
}
