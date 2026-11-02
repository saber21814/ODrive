// The real ConfigManager is exercised unchanged. Only its NVM storage backend
// is replaced; these tests do not model STM32 flash programming or power loss.
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include "MotorControl/nvm_config.hpp"

namespace {
constexpr size_t capacity = 256;
std::vector<uint8_t> committed, pending;
std::vector<bool> written;
bool writing = false;
size_t write_budget = capacity;
unsigned writes = 0, starts = 0, commits = 0, successful_reads = 0;
unsigned checks = 0, failures = 0;
struct TestFailure {};
void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL nvm_tests.cpp:%d: %s\n", line, expression);
        throw TestFailure{};
    }
}
#define CHECK(condition) check(bool(condition), #condition, __LINE__)

// Two objects deliberately have no padding. Foreign/current images have the
// exact same object sizes and bytes, so a version failure cannot be a length
// mismatch or a difference between one-axis and two-axis configuration structs.
using First = std::array<uint8_t, 8>;
using Second = std::array<uint8_t, 9>;
const First first_value{{0x12, 0x34, 0x00, 0xff, 0xa5, 0x5a, 0x80, 0x01}};
const Second second_value{{0x37, 0xc8, 0x91, 0x00, 0x08, 0xff, 0x5b, 0x10, 0xee}};
constexpr size_t payload_size = sizeof(First) + sizeof(Second);
constexpr size_t image_size = payload_size + sizeof(uint16_t);
static_assert(payload_size == 17, "Test payload must not contain padding");

void reset_storage() {
    committed.clear(); pending.clear(); written.clear(); writing = false;
    write_budget = capacity;
    writes = starts = commits = successful_reads = 0;
}
void power_cycle_mock() {
    // Discard an uncommitted write, retaining the last committed image.
    pending.clear(); written.clear(); writing = false; write_budget = capacity;
}
std::vector<uint8_t> image_for_version(uint16_t version) {
    std::vector<uint8_t> bytes(image_size);
    std::memcpy(bytes.data(), first_value.data(), sizeof(First));
    std::memcpy(bytes.data() + sizeof(First), second_value.data(), sizeof(Second));
    // Explicit version argument is independent of the build's config_version.
    // Version only changes the CRC seed: there is no new header or partition.
    const uint16_t crc = calc_crc16<0x3d65>(uint16_t(0xabcd ^ version),
                                          bytes.data(), payload_size);
    std::memcpy(bytes.data() + payload_size, &crc, sizeof(crc));
    return bytes;
}
struct LoadResult {
    bool started = false, first = false, second = false, finished = false;
    size_t occupied = 0;
    First first_data{};
    Second second_data{};
};
LoadResult load_current() {
    ConfigManager manager;
    LoadResult result;
    result.started = manager.start_load();
    result.first = manager.read(&result.first_data);
    result.second = manager.read(&result.second_data);
    result.finished = manager.finish_load(&result.occupied);
    return result;
}
void prepare_current(ConfigManager& manager) {
    First first = first_value;
    Second second = second_value;
    const unsigned old_writes = writes, old_starts = starts;
    CHECK(manager.prepare_store());
    CHECK(manager.write(&first));
    CHECK(manager.write(&second));
    CHECK(writes == old_writes && starts == old_starts); // preparation is dry
    size_t occupied = 0;
    CHECK(manager.start_store(&occupied));
    CHECK(occupied == image_size && pending.size() == image_size);
}
void store_current() {
    ConfigManager manager;
    prepare_current(manager);
    First first = first_value;
    Second second = second_value;
    CHECK(manager.write(&first));
    CHECK(manager.write(&second));
    CHECK(manager.finish_store());
    CHECK(manager.store_state == ConfigManager::kStoreStateIdle);
}
void check_valid_load() {
    const auto result = load_current();
    CHECK(result.started && result.first && result.second && result.finished);
    CHECK(result.first_data == first_value && result.second_data == second_value);
    CHECK(result.occupied == image_size);
}
void test_current_format_round_trip() {
    reset_storage();
#if defined(REHAB_V13)
    CHECK(config_version == 0x1301);
#elif defined(REHAB_V12)
    CHECK(config_version == 0x1201);
#else
    CHECK(config_version == 0x0001);
#endif
    store_current();
    CHECK(starts == 1 && writes == 3 && commits == 1);
    CHECK(committed == image_for_version(config_version));
    check_valid_load();
}
void test_equal_payload_version_isolation() {
    reset_storage();
    const auto original = image_for_version(0x0001);
    const auto rehab = image_for_version(0x1201);
    CHECK(original.size() == rehab.size() && original.size() == image_size);
    CHECK(std::equal(original.begin(), original.begin() + payload_size, rehab.begin()));
    CHECK(!std::equal(original.begin() + payload_size, original.end(), rehab.begin() + payload_size));
    for (const uint16_t version : {uint16_t(0x0001), uint16_t(0x1201), uint16_t(0x1301)}) {
        committed = image_for_version(version);
        successful_reads = 0;
        const auto result = load_current();
        CHECK(result.started && result.first && result.second);
        CHECK(result.first_data == first_value && result.second_data == second_value);
        CHECK(result.occupied == image_size && successful_reads == 3);
        // Both payload objects AND the CRC were read successfully. Only the
        // final seed-dependent CRC comparison distinguishes the two versions.
        CHECK(result.finished == (version == config_version));
    }
}
void test_every_truncated_image() {
    reset_storage();
    const auto good = image_for_version(config_version);
    for (size_t length = 0; length < good.size(); ++length) {
        committed.assign(good.begin(), good.begin() + length);
        const auto result = load_current();
        CHECK(result.started && !result.finished);
    }
    committed = good;
    check_valid_load();
}
void test_corruption() {
    reset_storage();
    const auto good = image_for_version(config_version);
    // Includes every payload bit and both CRC bytes. Reads must still complete;
    // corruption is rejected by the real finish_load CRC check, not the mock.
    for (size_t byte = 0; byte < good.size(); ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            committed = good;
            committed[byte] ^= uint8_t(1u << bit);
            successful_reads = 0;
            const auto result = load_current();
            CHECK(result.started && result.first && result.second && successful_reads == 3);
            CHECK(!result.finished && result.occupied == image_size);
        }
    }
}
void test_interrupted_write() {
    reset_storage();
    // Interrupted first-ever save does not publish a valid configuration.
    ConfigManager first_save;
    prepare_current(first_save);
    First first = first_value;
    CHECK(first_save.write(&first));
    power_cycle_mock();
    CHECK(committed.empty() && commits == 0);
    CHECK(!load_current().finished);

    store_current();
    const auto old_image = committed;
    // A short write fails in the second object and must not reach commit.
    ConfigManager replacement;
    prepare_current(replacement);
    write_budget = sizeof(First) + 3;
    CHECK(replacement.write(&first));
    Second second = second_value;
    CHECK(!replacement.write(&second));
    CHECK(!replacement.finish_store());
    CHECK(replacement.store_state == ConfigManager::kStoreStateFailed);
    CHECK(commits == 1 && committed == old_image);
    power_cycle_mock();
    check_valid_load();

    // Interrupt after the payload but before its CRC/commit. The mock's atomic
    // commit contract preserves the earlier image; real flash behavior remains
    // a hardware/backend concern, separate from ConfigManager acceptance.
    ConfigManager before_crc;
    prepare_current(before_crc);
    CHECK(before_crc.write(&first) && before_crc.write(&second));
    power_cycle_mock();
    CHECK(commits == 1 && committed == old_image);
    check_valid_load();
}
} // namespace

extern "C" {
int NVM_init(void) { return 0; }
int NVM_erase(void) { reset_storage(); return 0; }
size_t NVM_get_max_read_length(void) { return committed.size(); }
size_t NVM_get_max_write_length(void) { return capacity; }
int NVM_read(size_t offset, uint8_t* data, size_t length) {
    if (offset > committed.size() || length > committed.size() - offset) return -1;
    std::memcpy(data, committed.data() + offset, length);
    ++successful_reads;
    return 0;
}
int NVM_start_write(size_t length) {
    if (length > capacity) return -1;
    pending.assign(length, 0xff); written.assign(length, false);
    writing = true; ++starts;
    return 0;
}
int NVM_write(size_t offset, uint8_t* data, size_t length) {
    ++writes;
    if (!writing || offset > pending.size() || length > pending.size() - offset) return -1;
    const size_t accepted = std::min(length, write_budget);
    std::memcpy(pending.data() + offset, data, accepted);
    std::fill(written.begin() + offset, written.begin() + offset + accepted, true);
    write_budget -= accepted;
    return accepted == length ? 0 : -1;
}
int NVM_commit(void) {
    if (!writing || std::find(written.begin(), written.end(), false) != written.end()) return -1;
    committed = pending; writing = false; ++commits;
    return 0;
}
void NVM_demo(void) {}
}

int main() {
    struct Test { const char* name; void (*run)(); } tests[] = {
        {"current format two-pass round trip", test_current_format_round_trip},
        {"same-payload version CRC isolation", test_equal_payload_version_isolation},
        {"all truncated images", test_every_truncated_image},
        {"payload and CRC corruption", test_corruption},
        {"interrupted write and commit boundary", test_interrupted_write},
    };
    for (const auto& test : tests) {
        try { test.run(); std::printf("PASS %s\n", test.name); }
        catch (const TestFailure&) { std::printf("FAIL %s\n", test.name); }
    }
    std::printf("NVM ConfigManager version 0x%04x: %u checks, %u failed; memory backend, no flash hardware\n",
                unsigned(config_version), checks, failures);
    return failures ? 1 : 0;
}
