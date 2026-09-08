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

    TEST_CASE("transaction slot advances only after successful completion") {
        uint8_t slot = 7;
        CHECK(as5600::transaction_is_status(slot));
        slot = as5600::transaction_slot_after_completion(slot, false);
        CHECK(slot == 7);
        CHECK(as5600::transaction_is_status(slot));
        slot = as5600::transaction_slot_after_completion(slot, true);
        CHECK(slot == 8);
        CHECK(!as5600::transaction_is_status(slot));
        CHECK(as5600::transaction_slot_after_completion(255, true) == 0);
    }

    TEST_CASE("failed status completion cannot be starved by angle slots") {
        uint8_t slot = 0;
        for (unsigned i = 0; i < 7; ++i) {
            CHECK(!as5600::transaction_is_status(slot));
            slot = as5600::transaction_slot_after_completion(slot, true);
        }
        CHECK(slot == 7);
        CHECK(as5600::transaction_is_status(slot));
        for (unsigned retry = 0; retry < 5; ++retry) {
            slot = as5600::transaction_slot_after_completion(slot, false);
            CHECK(slot == 7);
            CHECK(as5600::transaction_is_status(slot));
        }
        slot = as5600::transaction_slot_after_completion(slot, true);
        CHECK(slot == 8);
        CHECK(!as5600::transaction_is_status(slot));
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

    TEST_CASE("normal maximum-speed samples are physically plausible") {
        // 1000 rpm is 16.67 turn/s: 34 counts in 500 us is expected.
        auto result = as5600::check_sample(1034, 1000, 104, 100,
                                           1.0f / 8000.0f, 20.0f);
        CHECK(result.raw_delta == 34);
        CHECK(result.max_abs_raw_delta == 56);
        CHECK(result.accepted);
    }

    TEST_CASE("a single large angle jump is rejected") {
        auto result = as5600::check_sample(1500, 1000, 102, 100,
                                           1.0f / 8000.0f, 20.0f);
        CHECK(result.raw_delta == 500);
        CHECK(result.max_abs_raw_delta == 30);
        CHECK(!result.accepted);
    }

    TEST_CASE("plausibility threshold has an absolute anti-glitch cap") {
        CHECK(as5600::max_plausible_raw_delta(
                  as5600::STALE_TICKS, 1.0f / 8000.0f, 20.0f) == 209);
        CHECK(as5600::max_plausible_raw_delta(
                  as5600::STALE_TICKS, 1.0f / 8000.0f, 30.0f) == 312);
        CHECK(as5600::max_plausible_raw_delta(
                  UINT32_MAX, 1.0f / 8000.0f, 30.0f) == 320);
    }

    TEST_CASE("zero crossing remains plausible") {
        auto result = as5600::check_sample(0, 4095, 102, 100,
                                           1.0f / 8000.0f, 20.0f);
        CHECK(result.raw_delta == 1);
        CHECK(result.accepted);
    }

    TEST_CASE("consecutive implausible samples reach the fault threshold") {
        uint8_t rejected = 0;
        for (uint8_t i = 0; i < as5600::MAX_CONSECUTIVE_REJECTED_SAMPLES; ++i) {
            auto result = as5600::check_sample((uint16_t)(1500 + i), 1000,
                                               (uint32_t)(102 + i), 100,
                                               1.0f / 8000.0f, 20.0f);
            REQUIRE(!result.accepted);
            rejected = as5600::update_failure_count(rejected, result.accepted);
        }
        CHECK(as5600::sample_rejection_failed(rejected));
    }

    TEST_CASE("plausibility timing handles uint32 tick wraparound") {
        auto result = as5600::check_sample(1034, 1000, 2, 0xfffffffeu,
                                           1.0f / 8000.0f, 20.0f);
        CHECK(result.max_abs_raw_delta == 56);
        CHECK(result.accepted);
    }

    TEST_CASE("successful recovery clears both recovery gates") {
        as5600::RecoveryState state = {true, true, 2, 1234};
        state = as5600::recovery_after_attempt(state, true, 2000);
        CHECK(!state.requested);
        CHECK(!state.blocked);
        CHECK(state.attempts == 0);
    }

    TEST_CASE("mismatched recovery gates are normalized and runnable") {
        as5600::RecoveryState state = {true, false, 0, 0};
        state = as5600::request_recovery(state, 500);
        CHECK(state.requested);
        CHECK(state.blocked);
        CHECK(state.attempts == 0);
        CHECK(as5600::recovery_attempt_due(state, 500));

        state = {false, true, 4, 900};
        state = as5600::request_recovery(state, 600);
        CHECK(state.requested);
        CHECK(state.blocked);
        CHECK(state.attempts == 0);
        CHECK(as5600::recovery_attempt_due(state, 600));
    }

    TEST_CASE("PLL feedback is gated by accepted samples and interval is bounded") {
        constexpr float dt = 1.0f / 8000.0f;
        CHECK(as5600::pll_feedback_period(false, 4, dt) == 0.0f);
        CHECK(as5600::pll_feedback_period(true, 4, dt) ==
              doctest::Approx(4.0f * dt));
        CHECK(as5600::pll_feedback_period(true, as5600::STALE_TICKS + 100u, dt) ==
              doctest::Approx((float)as5600::STALE_TICKS * dt));
    }

    TEST_CASE("failed recovery is retryable after cooldown and clear") {
        as5600::RecoveryState state = {true, true, 0, 100};
        state = as5600::recovery_after_attempt(state, false, 100);
        CHECK(state.requested);
        CHECK(state.blocked);
        CHECK(!as5600::recovery_attempt_due(state, state.next_tick - 1));
        CHECK(as5600::recovery_attempt_due(state, state.next_tick));

        for (uint8_t i = state.attempts;
             i < as5600::MAX_RECOVERY_ATTEMPTS; ++i) {
            state = as5600::recovery_after_attempt(state, false,
                                                   state.next_tick);
        }
        CHECK(!as5600::recovery_attempt_due(state, state.next_tick));
        state = as5600::rearm_recovery_after_clear(state, true,
                                                   state.next_tick);
        CHECK(state.requested);
        CHECK(state.blocked);
        CHECK(state.attempts == 0);
        CHECK(as5600::recovery_attempt_due(state, state.next_tick));
    }

    TEST_CASE("recovery deadline comparison handles uint32 tick wraparound") {
        as5600::RecoveryState state = {true, true, 1, 2};
        CHECK(!as5600::recovery_attempt_due(state, 0xfffffffeu));
        CHECK(as5600::recovery_attempt_due(state, 2));
    }
    TEST_CASE("recovery re-anchor accepts large legal motion and invalidates multi-turn") {
        auto r = as5600::recovery_reanchor(5000, 1300, 1000);
        CHECK(r.raw_delta == 300);
        CHECK(r.resumed_shadow == 5300);
        CHECK(!r.multiturn_valid);
    }

    TEST_CASE("recovery re-anchor crosses the AS5600 zero boundary") {
        auto r = as5600::recovery_reanchor(8191, 2, 4095);
        CHECK(r.raw_delta == 3);
        CHECK(r.resumed_shadow == 8194);
        CHECK(!r.multiturn_valid);
    }

    TEST_CASE("recovery movement beyond half a turn is explicitly ambiguous") {
        // From 1000 to 3500 the AS5600 alone cannot distinguish +2500 counts
        // from the nearer -1596-count path. Preserve the nearest path but mark
        // multi-turn state invalid so an application cannot mistake it for an
        // absolute multi-turn reconstruction.
        auto r = as5600::recovery_reanchor(10000, 3500, 1000);
        CHECK(r.raw_delta == -1596);
        CHECK(r.resumed_shadow == 8404);
        CHECK(!r.multiturn_valid);
    }

}
