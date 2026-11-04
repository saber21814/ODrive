#include "Rehab/platform/calibration_capture.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <initializer_list>
using namespace rehab::capture;
static Record sample(float bus, bool valid = true) {
    Record r;
    r.words[3] = valid ? uint32_t(ModelValid) : 0u;
    r.words[11] = float_word(bus);
    return r;
}
static uint32_t row(const Recorder& r, unsigned n, unsigned field = 0) {
    return r.word(kDataOffset + n * kRecordWords + field);
}
int main() {
    static_assert(sizeof(Record) == 112, "bounded record");
    static_assert(sizeof(Recorder) <= 64 * 112 + 32, "bounded storage");
    Recorder r;
    assert(r.word(0) == 1 && r.state() == 0 && r.word(UINT32_MAX) == 0);
    r.push(sample(-1)); assert(r.word(3) == 0);
    r.arm(); assert(r.word(2) == 1 && r.word(5) == UINT32_MAX);
    r.finish(); assert(r.state() == 1); // No fabricated sample on empty finish.
    r.push(sample(-.01f));
    r.push(sample(-.009f));
    r.push(sample(-1, false));
    r.push(sample(std::numeric_limits<float>::quiet_NaN()));
    r.push(sample(-std::numeric_limits<float>::infinity()));
    assert(r.state() == 1 && row(r, 0) == 0); // Live rows never exposed.
    r.push(sample(-.0101f));
    assert(r.state() == 2 && r.word(4) == 1 && r.word(3) == 6 && r.word(5) == 5);
    assert(row(r, 0) == 1 && row(r, 5) == 6);
    auto frozen = row(r, 5, 11);
    r.push(sample(-9)); r.finish();
    assert(row(r, 5, 11) == frozen && r.word(3) == 6 && r.word(4) == 1);
    assert(r.word(9) == 0 && r.word(15) == 0 && row(r, 6) == 0 && r.word(UINT32_MAX) == 0);
    r.arm(); assert(r.word(2) == 2 && r.word(3) == 0 && r.word(4) == 0);
    for (unsigned i = 0; i < 80; ++i) r.push(sample(0));
    r.push(sample(-.02f));
    assert(r.word(3) == 64 && r.word(5) == 63 && row(r, 0) == 18 && row(r, 63) == 81);
    for (unsigned field : {19u, 20u, 21u}) {
        r.arm(); auto s = sample(0); s.words[field] = 8; r.push(s);
        assert(r.word(4) == 2 && r.word(5) == 0);
    }
    r.arm(); auto s = sample(-.1f); s.words[19] = 1; r.push(s);
    assert(r.word(4) == 1); // First negative estimate takes precedence over its fault.
    r.arm(); r.push(sample(0)); r.finish();
    assert(r.word(4) == 3 && r.word(5) == UINT32_MAX && row(r, 0) == 1);
    pending = Record{};
    assert(std::isnan(word_float(pending.words[8])));
    pending.words[3] = RawValid | PwmAtSample | ArmedAtSample;
    active = false; model_measure(1, 1, 2, 3, 4, true); assert(pending.words[2] == 0);
    active = true; model_measure(1, 1, 2, 3, 4, true);
    assert(pending.words[2] == 1 && (pending.words[3] & CurrentValid));
    assert((pending.words[3] & (RawValid | PwmAtSample | ArmedAtSample)) ==
           (RawValid | PwmAtSample | ArmedAtSample));
    assert(word_float(pending.words[8]) == 1 && word_float(pending.words[9]) == 2);
    model_output(1, 5, -.02f, 123, true);
    assert((pending.words[3] & (ModelValid | OutputValid)) == (ModelValid | OutputValid));
    assert(word_float(pending.words[10]) == 5 && pending.words[25] == 123);
    model_output(1, 9, 9, 124, false);
    assert(!(pending.words[3] & (ModelValid | OutputValid)));
    r.arm(); r.push(pending); assert(r.state() == 1); // Invalid stale negative cannot trigger.
    model_measure(2, 0, 0, 0, 0, false); assert(!(pending.words[3] & CurrentValid));
    active = false;
    std::puts("PASS calibration capture: trigger/NaN/flags, ring ordering, freeze, faults, rearm and bounds");
}
