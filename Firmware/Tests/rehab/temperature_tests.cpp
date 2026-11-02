#include "Rehab/platform/temperature.hpp"
#include "Rehab/platform/commissioning.hpp"
#include <cassert>
#include <limits>
#include <initializer_list>
#include <cstdio>
using rehab::platform::temperature_from_ratio;
static float ratio_at(float c) {
    const float resistance=10000.0f*std::exp(3950.0f*(1.0f/(c+273.15f)-1.0f/298.15f));
    return 3300.0f/(3300.0f+resistance);
}
int main() {
    static_assert(rehab::commissioning::ntc_beta==3950.0f,"V1.3 nominal NTC profile");
    static_assert(!rehab::commissioning::may_energize,"Nominal NTC must not unlock motion");
    // Datasheet nominal R25=10k and beta curve, not a claim of physical calibration.
    assert(std::fabs(temperature_from_ratio(3300.0f/13300.0f,10000,3950)-25)<0.001f);
    for(float c : {-20.0f,0.0f,25.0f,50.0f,80.0f,120.0f})
        assert(std::fabs(temperature_from_ratio(ratio_at(c),10000,3950)-c)<0.002f);
    for(float ratio : {0.0f,1.0f,-1.0f,2.0f,0.001f,0.999f,
        std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        assert(!std::isfinite(temperature_from_ratio(ratio,10000,3950)));
    assert(!std::isfinite(temperature_from_ratio(0.5f,10000,0)));
    assert(!std::isfinite(temperature_from_ratio(0.5f,-1,3950)));
    std::puts("PASS V1.3 nominal NTC, open/short/nonfinite input and independent motion lock");
}
