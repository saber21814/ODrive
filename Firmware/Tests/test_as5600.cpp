#include <doctest.h>
#include "MotorControl/as5600_utils.hpp"

TEST_SUITE("AS5600 helpers") {
    TEST_CASE("12-bit angle decoding") {
        CHECK(as5600::decode_raw_angle(0x0f, 0xa5) == 0xfa5);
        CHECK(as5600::decode_raw_angle(0xf0, 0x12) == 0x012);
    }

    TEST_CASE("wrapped deltas and multi-turn direction") {
        CHECK(as5600::wrapped_delta(2, 4094) == 4);
        CHECK(as5600::wrapped_delta(4094, 2) == -4);
        // A repeated sample must not advance the multi-turn count.
        CHECK(as5600::wrapped_delta(1000, 1000) == 0);
        int32_t multi = 0;
        multi += as5600::wrapped_delta(4090, 4090);
        multi += as5600::wrapped_delta(2, 4090);
        multi += as5600::wrapped_delta(10, 2);
        CHECK(multi == 16);
        multi += as5600::wrapped_delta(4090, 10);
        CHECK(multi == 0);
        CHECK(as5600::wrapped_delta(2048, 0) == 2048);
        CHECK(as5600::wrapped_delta(0, 2048) == -2048);
    }

    TEST_CASE("volatile configuration preserves unrelated fields") {
        uint16_t configured = as5600::configure_conf(0x20fc);
        CHECK((configured & (3u << 0)) == 0);
        CHECK((configured & (3u << 8)) == (3u << 8));
        CHECK((configured & (7u << 10)) == (1u << 10));
        CHECK((configured & (1u << 13)) == 0);
        CHECK((configured & (3u << 2)) == (0x20fc & (3u << 2)));
    }

    TEST_CASE("magnet status and recovery error inputs") {
        CHECK(as5600::magnet_detected(0x20));
        CHECK(!as5600::magnet_detected(0x18));
        CHECK(as5600::magnet_too_weak(0x10));
        CHECK(as5600::magnet_too_strong(0x08));
        CHECK(as5600::wrapped_delta(0, 4095) == 1);
    }

    TEST_CASE("NACK threshold, timeout and bus recovery bounds") {
        CHECK(!as5600::communication_failed(1));
        CHECK(!as5600::communication_failed(2));
        CHECK(as5600::communication_failed(3));
        CHECK(!as5600::sample_is_stale(116, 100));
        CHECK(as5600::sample_is_stale(117, 100));
        CHECK(as5600::sample_is_stale(5, 0xfffffff0u));
        CHECK(!as5600::recovery_clocks_complete(8));
        CHECK(as5600::recovery_clocks_complete(9));
    }
}
