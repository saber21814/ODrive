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

    TEST_CASE("seven angle slots are followed by one status slot") {
        unsigned angle_slots = 0;
        unsigned status_slots = 0;
        for (uint8_t slot = 0; slot < 32; ++slot) {
            if (as5600::transaction_is_status(slot)) {
                ++status_slots;
            } else {
                ++angle_slots;
            }
        }
        CHECK(angle_slots == 28);
        CHECK(status_slots == 4);
        CHECK(!as5600::transaction_is_status(6));
        CHECK(as5600::transaction_is_status(7));
        CHECK(!as5600::transaction_is_status(8));
        CHECK(as5600::transaction_is_status(15));
    }

    TEST_CASE("recovery resumes the existing multi-turn coordinate") {
        CHECK(as5600::resume_multiturn(8190, 2, 4094) == 8194);
        CHECK(as5600::resume_multiturn(-8190, 4094, 2) == -8194);
        CHECK(as5600::resume_multiturn(12345, 1000, 1000) == 12345);
    }

    TEST_CASE("transaction admission never enters HAL while unsafe") {
        CHECK(as5600::transaction_can_start(false, false, false, true, false));
        CHECK(!as5600::transaction_can_start(true, false, false, true, false));
        CHECK(!as5600::transaction_can_start(false, true, false, true, false));
        CHECK(!as5600::transaction_can_start(false, false, true, true, false));
        CHECK(!as5600::transaction_can_start(false, false, false, false, false));
        CHECK(!as5600::transaction_can_start(false, false, false, true, true));
    }

    TEST_CASE("recovery only runs disarmed with a valid axis") {
        CHECK(as5600::recovery_can_run(true, true, false));
        CHECK(!as5600::recovery_can_run(false, true, false));
        CHECK(!as5600::recovery_can_run(true, false, false));
        CHECK(!as5600::recovery_can_run(true, true, true));
    }

    TEST_CASE("three consecutive failures trigger and success clears") {
        uint8_t failures = 0;
        failures = as5600::update_failure_count(failures, false);
        CHECK(failures == 1);
        CHECK(!as5600::communication_failed(failures));
        failures = as5600::update_failure_count(failures, false);
        CHECK(failures == 2);
        CHECK(!as5600::communication_failed(failures));
        failures = as5600::update_failure_count(failures, false);
        CHECK(failures == 3);
        CHECK(as5600::communication_failed(failures));
        failures = as5600::update_failure_count(failures, true);
        CHECK(failures == 0);
        CHECK(as5600::update_failure_count(UINT8_MAX, false) == UINT8_MAX);
    }

    TEST_CASE("status freshness uses a strict ten millisecond boundary") {
        CHECK(!as5600::status_is_stale(180, 100));
        CHECK(as5600::status_is_stale(181, 100));
        CHECK(!as5600::status_is_stale(0x40, 0xfffffff0u));
        CHECK(as5600::status_is_stale(0x41, 0xfffffff0u));
    }
}
