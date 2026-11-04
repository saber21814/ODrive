#include "calibration_capture.hpp"
#include <cstring>
#include <limits>

namespace rehab { namespace capture {
uint32_t float_word(float value) {
    static_assert(sizeof(float) == sizeof(uint32_t), "32-bit floats required");
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
float word_float(uint32_t value) {
    float result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
Record::Record() : words{} {
    for (unsigned i = 8; i <= 18; ++i)
        words[i] = float_word(std::numeric_limits<float>::quiet_NaN());
}
Recorder recorder;
Record pending;
bool active = false;

void Recorder::arm() {
    // Old storage remains inaccessible until replaced; no bulk ISR-blocking clear.
    state_ = 1;
    ++generation_;
    count_ = next_ = sequence_ = reason_ = 0;
}
void Recorder::push(Record sample) {
    if (state_ != 1) return;
    sample.words[0] = ++sequence_;
    samples_[next_] = sample;
    next_ = (next_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
    if (sample.words[19] || sample.words[20] || sample.words[21])
        reason_ = 2;
    if (reason_) state_ = 2;
}
void Recorder::finish() {
    if (state_ == 1 && count_) {
        reason_ = 3;
        state_ = 2;
    }
}
uint32_t Recorder::word(unsigned index) const {
    switch (index) {
        case 0: return 1;
        case 1: return state_;
        case 2: return generation_;
        case 3: return count_;
        case 4: return reason_;
        case 5: return (reason_ == 1 || reason_ == 2) ? count_ - 1 : UINT32_MAX;
        case 6: return kCapacity;
        case 7: return kRecordWords;
        case 8: return float_word(kNegativeThreshold);
        default: break;
    }
    if (state_ != 2 || index < kDataOffset) return 0;
    const unsigned offset = index - kDataOffset;
    const unsigned row = offset / kRecordWords;
    if (row >= count_) return 0;
    const unsigned first = (next_ + kCapacity - count_) % kCapacity;
    return samples_[(first + row) % kCapacity].words[offset % kRecordWords];
}
void model_measure(unsigned phase, float alpha, float beta, float voltage,
                   float bus, bool valid) {
    if (!active) return;
    pending.words[2] = phase;
    pending.words[3] &= ~(CurrentValid | ModelValid | OutputValid);
    if (valid) {
        pending.words[3] |= CurrentValid | ModelValid;
        pending.words[8] = float_word(alpha);
        pending.words[9] = float_word(beta);
        pending.words[10] = float_word(voltage);
        pending.words[11] = float_word(bus);
    }
}
void model_output(unsigned phase, float voltage, float bus,
                  uint32_t timestamp, bool valid) {
    if (!active) return;
    pending.words[2] = phase;
    pending.words[25] = timestamp;
    pending.words[3] &= ~(ModelValid | OutputValid);
    if (valid) {
        pending.words[3] |= ModelValid | OutputValid;
        pending.words[10] = float_word(voltage);
        pending.words[11] = float_word(bus);
    }
}
}}
