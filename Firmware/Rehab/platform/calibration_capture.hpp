#pragma once
#include <cstdint>

namespace rehab { namespace capture {
constexpr unsigned kRecordWords = 28;
constexpr unsigned kCapacity = 64;
constexpr unsigned kDataOffset = 16;
constexpr float kNegativeThreshold = -0.01f;
enum Flags : uint32_t {
    RawValid = 1, CurrentValid = 2, ModelValid = 4,
    PwmAtSample = 8, ArmedAtSample = 16, OutputValid = 32
};
uint32_t float_word(float value);
float word_float(uint32_t value);
struct Record {
    uint32_t words[kRecordWords];
    // Unknown float fields are NaN, never mistaken for measured zeros.
    Record();
};

// ISR is the sole writer. Board-side task readers/arm() must mask interrupts.
// No hardware I/O, allocation, control commands or protection mutations here.
class Recorder {
public:
    void arm();
    void push(Record sample);
    void finish();
    uint32_t word(unsigned index) const;
    uint32_t state() const { return state_; }
private:
    Record samples_[kCapacity];
    uint32_t state_ = 0; // 0 unarmed, 1 recording, 2 immutable frozen
    uint32_t generation_ = 0;
    uint32_t count_ = 0;
    uint32_t next_ = 0;
    uint32_t sequence_ = 0;
    uint32_t reason_ = 0; // 1 negative estimate, 2 protection error, 3 end
};
extern Recorder recorder;
extern Record pending;
extern bool active;
void model_measure(unsigned phase, float alpha, float beta, float voltage,
                   float bus, bool valid);
void model_output(unsigned phase, float voltage, float bus,
                  uint32_t timestamp, bool valid);
}}
