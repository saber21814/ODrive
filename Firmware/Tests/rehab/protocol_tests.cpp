#include "Rehab/protocol/frame.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace rehab::wire;
using Bytes = std::vector<uint8_t>;
using Decision = Replay::Decision;
size_t assertions = 0;

void require(bool condition, const char* expression, int line) {
    ++assertions;
    if (!condition) {
        throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
    }
}
#define CHECK(expression) require(bool(expression), #expression, __LINE__)

Frame request(uint32_t sequence = 1, uint16_t length = 8) {
    Frame frame;
    frame.command = static_cast<uint16_t>(Command::SetConfig);
    frame.session = 0x12345678u;
    frame.sequence = sequence;
    frame.length = length;
    for (size_t i = 0; i < length && i < kPayload; ++i) {
        frame.payload[i] = static_cast<uint8_t>(i * 37 + sequence);
    }
    return frame;
}

Frame response_for(const Frame& frame) {
    Frame response = frame;
    response.flags = 1;
    response.length = 4;
    put32(response.payload, frame.sequence ^ 0xa5a5a5a5u);
    return response;
}

bool equal(const Frame& a, const Frame& b) {
    return a.flags == b.flags && a.command == b.command && a.length == b.length &&
        a.session == b.session && a.sequence == b.sequence &&
        a.length <= kPayload && std::memcmp(a.payload, b.payload, a.length) == 0;
}

Bytes encoded(const Frame& frame) {
    Bytes result(kMaxFrame);
    const size_t length = encode(frame, result.data(), result.size());
    CHECK(length != 0);
    result.resize(length);
    return result;
}

void append(Bytes& destination, const Bytes& bytes) {
    destination.insert(destination.end(), bytes.begin(), bytes.end());
}

struct Sink {
    std::vector<Frame> frames;
    static void receive(void* context, const Frame& frame) {
        static_cast<Sink*>(context)->frames.push_back(frame);
    }
};

void consume(Parser& parser, Sink& sink, const Bytes& bytes, uint32_t now = 0) {
    parser.consume(bytes.data(), bytes.size(), now, &Sink::receive, &sink);
}

void crc_and_encoding_bounds() {
    const uint8_t known[] = {'1','2','3','4','5','6','7','8','9'};
    CHECK(crc32(known, sizeof(known)) == 0xcbf43926u);
    CHECK(crc32(nullptr, 0) == 0u);
    auto frame = request(0x89abcdefu, 0);
    const auto bytes = encoded(frame);
    CHECK(bytes.size() == 20);
    CHECK(bytes[0] == 'R' && bytes[1] == 'H' && bytes[2] == 1 && bytes[3] == 0);
    CHECK(u16(bytes.data() + 4) == 0);
    CHECK(u16(bytes.data() + 6) == static_cast<uint16_t>(Command::SetConfig));
    CHECK(u32(bytes.data() + 8) == frame.session);
    CHECK(u32(bytes.data() + 12) == frame.sequence);
    CHECK(u32(bytes.data() + 16) == crc32(bytes.data(), 16));
    std::array<uint8_t, kMaxFrame + 2> guarded;
    guarded.fill(0xa7);
    frame = request(1, kPayload);
    CHECK(encode(frame, guarded.data() + 1, kMaxFrame) == kMaxFrame);
    CHECK(guarded.front() == 0xa7 && guarded.back() == 0xa7);
    guarded.fill(0xa7);
    CHECK(encode(frame, guarded.data() + 1, kMaxFrame - 1) == 0);
    CHECK(std::all_of(guarded.begin(), guarded.end(), [](uint8_t b) { return b == 0xa7; }));
    frame.length = kPayload + 1;
    CHECK(encode(frame, guarded.data() + 1, kMaxFrame) == 0);
    frame.length = 0;
    frame.flags = 2;
    CHECK(encode(frame, guarded.data() + 1, kMaxFrame) == 0);
    CHECK(std::all_of(guarded.begin(), guarded.end(), [](uint8_t b) { return b == 0xa7; }));
}

void every_split_point_and_bytewise() {
    for (uint16_t length : {0, 1, 15, 64, 256}) {
        const auto frame = request(1, length);
        const auto bytes = encoded(frame);
        for (size_t split = 0; split <= bytes.size(); ++split) {
            Parser parser;
            Sink sink;
            parser.consume(bytes.data(), split, 100, &Sink::receive, &sink);
            CHECK(sink.frames.size() == (split == bytes.size() ? 1u : 0u));
            parser.consume(bytes.data() + split, bytes.size() - split, 101,
                           &Sink::receive, &sink);
            CHECK(sink.frames.size() == 1);
            CHECK(equal(sink.frames.front(), frame));
            CHECK(parser.valid == 1 && parser.bad_crc == 0 && parser.bad_header == 0);
        }
        Parser parser;
        Sink sink;
        for (size_t i = 0; i < bytes.size(); ++i) {
            parser.consume(bytes.data() + i, 1, static_cast<uint32_t>(i),
                           &Sink::receive, &sink);
        }
        CHECK(sink.frames.size() == 1 && equal(sink.frames[0], frame));
    }
}

void concatenation_and_garbage() {
    Parser parser;
    Sink sink;
    Bytes bytes = {0, 0xff, 'R', 'R', 'H', 0x7f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::vector<Frame> expected;
    for (uint32_t i = 1; i <= 40; ++i) {
        expected.push_back(request(i, static_cast<uint16_t>((i * 23) % 257)));
        append(bytes, encoded(expected.back()));
        bytes.insert(bytes.end(), {0xff, 0, 'R'});
    }
    consume(parser, sink, bytes);
    CHECK(sink.frames.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) CHECK(equal(sink.frames[i], expected[i]));
    CHECK(parser.valid == expected.size());
    CHECK(parser.bad_header >= 1);
}

void malformed_headers_and_crc() {
    const auto good = request(77, 3);
    for (unsigned error = 0; error < 5; ++error) {
        Parser parser;
        Sink sink;
        Bytes bad = encoded(request(8, 0));
        if (error == 0) bad[2] = kVersion + 1;
        if (error == 1) bad[3] = 2;
        if (error == 2) put16(bad.data() + 4, kPayload + 1);
        if (error == 3) put16(bad.data() + 4, 0xffff);
        if (error == 4) bad.back() ^= 0x80;
        append(bad, encoded(good));
        consume(parser, sink, bad);
        CHECK(sink.frames.size() == 1 && equal(sink.frames[0], good));
        CHECK(error == 4 ? parser.bad_crc >= 1 : parser.bad_header >= 1);
    }
}

void embedded_magic_resynchronization() {
    const auto inner = request(88, 7);
    const auto inner_bytes = encoded(inner);
    auto outer = request(9, static_cast<uint16_t>(inner_bytes.size() + 6));
    std::fill(std::begin(outer.payload), std::end(outer.payload), 0);
    // A misleading RH prefix precedes a complete nested valid frame. A bad
    // outer CRC must not discard the entire body and lose the nested frame.
    outer.payload[0] = 'R'; outer.payload[1] = 'H'; outer.payload[2] = 0xff;
    std::copy(inner_bytes.begin(), inner_bytes.end(), outer.payload + 4);
    Bytes bytes = encoded(outer);
    bytes.back() ^= 1;
    const auto after = request(89, 0);
    append(bytes, encoded(after));
    Parser parser;
    Sink sink;
    consume(parser, sink, bytes);
    CHECK(sink.frames.size() == 2);
    CHECK(equal(sink.frames[0], inner) && equal(sink.frames[1], after));
    CHECK(parser.bad_crc >= 1 && parser.bad_header >= 1);
}

void partial_timeout_and_clock_wrap() {
    const auto frame = request(1, 32);
    const auto bytes = encoded(frame);
    Parser parser;
    Sink sink;
    parser.consume(bytes.data(), 10, 100, &Sink::receive, &sink);
    parser.expire(350); // Current protocol: age >250ms expires, equality survives.
    CHECK(parser.expired == 0 && sink.frames.empty());
    parser.consume(bytes.data() + 10, bytes.size() - 10, 350, &Sink::receive, &sink);
    CHECK(sink.frames.size() == 1 && equal(sink.frames[0], frame));

    parser.consume(bytes.data(), kHeader + 1, 500, &Sink::receive, &sink);
    parser.expire(751);
    CHECK(parser.expired == 1);
    parser.expire(1000);
    CHECK(parser.expired == 1); // Expiration is reported once per partial frame.
    consume(parser, sink, bytes, 1001);
    CHECK(sink.frames.size() == 2);

    Parser wrapped;
    Sink wrap_sink;
    constexpr uint32_t started = 0xffffff80u;
    wrapped.consume(bytes.data(), 8, started, &Sink::receive, &wrap_sink);
    wrapped.expire(started + 250u);
    CHECK(wrapped.expired == 0);
    consume(wrapped, wrap_sink, bytes, started + 251u); // consume also expires.
    CHECK(wrapped.expired == 1 && wrap_sink.frames.size() == 1);
    CHECK(equal(wrap_sink.frames[0], frame));
    wrapped.reset();
    wrapped.consume(bytes.data(), 5, 5000, &Sink::receive, &wrap_sink);
    wrapped.reset();
    consume(wrapped, wrap_sink, bytes, 5001);
    CHECK(wrap_sink.frames.size() == 2);
}

void sequence_zero_maximum_no_wrap() {
    Replay replay;
    Frame cached;
    auto zero = request(0);
    CHECK(replay.check(zero, cached) == Decision::Reject); // No negotiated session.
    replay.reset(zero.session);
    CHECK(replay.check(zero, cached) == Decision::Fresh);
    replay.remember(zero, response_for(zero));
    CHECK(replay.check(zero, cached) == Decision::Cached);
    CHECK(equal(cached, response_for(zero)));
    replay.reset(zero.session);
    auto highest = request(std::numeric_limits<uint32_t>::max());
    CHECK(replay.check(highest, cached) == Decision::Fresh);
    replay.remember(highest, response_for(highest));
    CHECK(replay.check(highest, cached) == Decision::Cached);
    CHECK(replay.check(zero, cached) == Decision::Reject);
    CHECK(replay.check(request(1), cached) == Decision::Reject);
    CHECK(replay.check(request(highest.sequence - 1), cached) == Decision::Reject);
    replay.reset(zero.session + 1);
    CHECK(replay.check(zero, cached) == Decision::Reject);
    zero.session += 1;
    CHECK(replay.check(zero, cached) == Decision::Fresh);
}

void four_response_cache_and_old_sequences() {
    Replay replay;
    Frame cached;
    replay.reset(request().session);
    for (uint32_t sequence = 1; sequence <= 5; ++sequence) {
        const auto frame = request(sequence);
        CHECK(replay.check(frame, cached) == Decision::Fresh);
        replay.remember(frame, response_for(frame));
    }
    CHECK(replay.check(request(1), cached) == Decision::Reject);
    CHECK(replay.check(request(0), cached) == Decision::Reject);
    for (uint32_t sequence = 2; sequence <= 5; ++sequence) {
        CHECK(replay.check(request(sequence), cached) == Decision::Cached);
        CHECK(equal(cached, response_for(request(sequence))));
    }
    CHECK(replay.check(request(7), cached) == Decision::Fresh);
    replay.remember(request(7), response_for(request(7)));
    CHECK(replay.check(request(6), cached) == Decision::Reject); // Old but never cached.
    CHECK(replay.check(request(2), cached) == Decision::Reject);
    CHECK(replay.check(request(8), cached) == Decision::Fresh);
}

void changed_request_wrong_session_and_response_flag() {
    Replay replay;
    const auto original = request(3, 8);
    Frame cached;
    replay.reset(original.session);
    replay.remember(original, response_for(original));
    auto changed = original;
    changed.command = static_cast<uint16_t>(Command::Start);
    CHECK(replay.check(changed, cached) == Decision::Reject);
    changed = original; changed.payload[7] ^= 0x80;
    CHECK(replay.check(changed, cached) == Decision::Reject);
    changed = original; changed.length = 7;
    CHECK(replay.check(changed, cached) == Decision::Reject);
    changed = original; changed.session += 1;
    CHECK(replay.check(changed, cached) == Decision::Reject);
    changed = original; changed.flags = 1;
    CHECK(replay.check(changed, cached) == Decision::Reject);
    changed.sequence = 4;
    CHECK(replay.check(changed, cached) == Decision::Reject);
    // Responses are valid framing, but cannot enter the request admission path.
    Parser parser;
    Sink sink;
    consume(parser, sink, encoded(changed));
    CHECK(sink.frames.size() == 1 && sink.frames[0].flags == 1);
    CHECK(replay.check(sink.frames[0], cached) == Decision::Reject);
    CHECK(replay.check(original, cached) == Decision::Cached);
    CHECK(equal(cached, response_for(original)));
    replay.reset(0);
    CHECK(replay.check(original, cached) == Decision::Reject);
}

void different_payload_crc_collision_must_not_replay() {
    // Independently constructed with Python zlib: both distinct 8-byte values
    // have CRC32 0x6522df69. A hash alone is not proof of request identity.
    auto first = request(50, 8);
    std::fill(std::begin(first.payload), std::end(first.payload), 0);
    auto second = first;
    const uint8_t collision[] = {0x52, 0x48, 0x01, 0x00, 0x10, 0x2a, 0x96, 0x06};
    std::memcpy(second.payload, collision, sizeof(collision));
    CHECK(crc32(first.payload, 8) == 0x6522df69u);
    CHECK(crc32(second.payload, 8) == 0x6522df69u);
    CHECK(!equal(first, second));
    Replay replay;
    Frame cached;
    replay.reset(first.session);
    replay.remember(first, response_for(first));
    CHECK(replay.check(second, cached) == Decision::Reject);
    CHECK(replay.check(first, cached) == Decision::Cached);
}

uint32_t random_word(uint32_t& state) {
    // Specified xorshift32, reproducible across standard-library versions.
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}

void fixed_seed_random_byte_pressure() {
    uint32_t seed = 0x5eed1234u;
    uint32_t now = 0xffff0000u;
    Parser parser;
    Sink sink;
    for (uint32_t sequence = 1; sequence <= 10000; ++sequence) {
        Bytes garbage(random_word(seed) % 300);
        for (auto& byte : garbage) byte = static_cast<uint8_t>(random_word(seed));
        consume(parser, sink, garbage, now);
        // Random junk may contain an incomplete plausible header. Its bounded
        // expiration must restore progress without requiring a parser restart.
        now += 251;
        parser.expire(now);
        const size_t before = sink.frames.size();
        auto frame = request(sequence, static_cast<uint16_t>(random_word(seed) % 257));
        for (size_t i = 0; i < frame.length; ++i) frame.payload[i] = uint8_t(random_word(seed));
        const auto bytes = encoded(frame);
        for (size_t offset = 0; offset < bytes.size();) {
            const size_t count = std::min<size_t>(1 + random_word(seed) % 23, bytes.size() - offset);
            parser.consume(bytes.data() + offset, count, ++now, &Sink::receive, &sink);
            offset += count;
        }
        CHECK(sink.frames.size() == before + 1);
        CHECK(equal(sink.frames.back(), frame));
    }
    CHECK(parser.valid == 10000);
    CHECK(sink.frames.size() == 10000);
}
}

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"CRC32 and encoder bounds", crc_and_encoding_bounds},
        {"all split points and bytewise input", every_split_point_and_bytewise},
        {"concatenated frames and garbage", concatenation_and_garbage},
        {"bad version, flags, lengths and CRC", malformed_headers_and_crc},
        {"embedded magic resynchronization", embedded_magic_resynchronization},
        {"partial timeout boundary and clock wrap", partial_timeout_and_clock_wrap},
        {"sequence zero, maximum and forbidden wrap", sequence_zero_maximum_no_wrap},
        {"four-entry response cache and old sequence refusal", four_response_cache_and_old_sequences},
        {"changed requests, sessions and response flags", changed_request_wrong_session_and_response_flag},
        {"CRC collision is not request identity", different_payload_crc_collision_must_not_replay},
        {"fixed-seed random byte pressure", fixed_seed_random_byte_pressure},
    };
    size_t failures = 0;
    for (const auto& test : tests) {
        try { test.run(); std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests)
              << " cases passed; " << assertions << " assertions\n";
    return failures ? 1 : 0;
}
