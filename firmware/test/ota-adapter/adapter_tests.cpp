// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/events.hpp"
#include "esp_ota_ops.h"
#include "mbedtls/sha256.h"
#include "ota/ingestion.hpp"
#include "ota/installation.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace prd::ota;
// Shim assertions can fire while the adapter mutex is held. Flush their evidence
// and exit without static destructors trying to reacquire that same mutex.
#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value);                       \
            std::fflush(stderr);                                                                   \
            std::_Exit(1);                                                                         \
        }                                                                                          \
    } while (false)

esp_partition_t running{0, 0x10, 0x20000, 0x200000};
esp_partition_t target{0, 0x11, 0x220000, 0x200000};
bool missing_running = false;
bool missing_target = false;
bool begin_error = false;
bool write_error = false;
bool start_error = false;
bool update_error = false;
bool finish_error = false;
bool live = false;
unsigned begins = 0;
unsigned writes = 0;
unsigned aborts = 0;
unsigned ends = 0;
unsigned reads = 0;
bool end_error = false;
bool read_error = false;
bool selection_error = false;
bool selection_write_error = false;
unsigned selections = 0;
const esp_partition_t* selected_target = nullptr;
std::vector<std::uint8_t> stored;

void reset() {
    device_ingestion_port().abort();
    running = {0, 0x10, 0x20000, 0x200000};
    target = {0, 0x11, 0x220000, 0x200000};
    missing_running = missing_target = begin_error = write_error = false;
    start_error = update_error = finish_error = live = false;
    begins = writes = aborts = 0;
    ends = reads = 0;
    end_error = read_error = false;
    selection_error = false;
    selection_write_error = false;
    selections = 0;
    selected_target = nullptr;
    stored.clear();
}

std::string hex(const ImageDigest& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

std::string sha(const std::uint8_t* bytes, std::size_t size) {
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    ImageDigest digest{};
    CHECK(mbedtls_sha256_starts(&context, 0) == 0);
    CHECK(mbedtls_sha256_update(&context, bytes, size) == 0);
    CHECK(mbedtls_sha256_finish(&context, digest.data()) == 0);
    mbedtls_sha256_free(&context);
    return hex(digest);
}

void known_vectors() {
    const std::string abc = "abc";
    CHECK(sha(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string long_text = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha(reinterpret_cast<const std::uint8_t*>(long_text.data()), long_text.size()) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha(nullptr, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// Starts with ESP image magic so the shim does not admit data real APP write
// immediately rejects. This is not a valid/signed ESP firmware image.
std::vector<std::uint8_t> payload() {
    std::vector<std::uint8_t> result(257);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::uint8_t>(index);
    }
    result[0] = 0xe9;
    return result;
}

void successful_chunks() {
    const auto bytes = payload();
    const auto digest = sha(bytes.data(), bytes.size());
    const Candidate candidate{1, "board", "esp32s3", "1.2.3", 1, bytes.size(), digest};
    const DeviceCompatibility device{"board", "esp32s3", "1.2.2", 1, 0x200000};
    for (const std::size_t chunk : {1U, 63U, 64U, 65U, 257U}) {
        reset();
        target.size = static_cast<std::uint32_t>(bytes.size());
        {
            ImageIngestion ingestion{device_ingestion_port()};
            CHECK(ingestion.begin(candidate, device, DowngradePolicy::Reject));
            CHECK(ingestion.append(nullptr, 0));
            for (std::size_t at = 0; at < bytes.size(); at += chunk) {
                CHECK(ingestion.append(bytes.data() + at, std::min(chunk, bytes.size() - at)));
            }
            CHECK(ingestion.finish());
            CHECK(ingestion.status() == IngestionStatus::DigestMatched);
            CHECK(stored == bytes);
            CHECK(live && aborts == 0);
            CHECK(!ingestion.finish());
        }
        CHECK(aborts == 1 && !live);
        device_ingestion_port().abort();
        CHECK(aborts == 1);
    }
}

void verified_handoff() {
    // Synthetic bytes and a mocked SDK end: these test binding/order/ownership,
    // NOT RSA signature verification or actual ESP image validation.
    for (unsigned scenario = 0; scenario < 15; ++scenario) {
        reset();
        std::vector<std::uint8_t> bytes(376, 0);
        bytes[0] = 0xe9;
        bytes[1] = 1;
        bytes[28] = 0x58;
        bytes[29] = 1; // first segment = 344 bytes
        bytes[32] = 0x32;
        bytes[33] = 0x54;
        bytes[34] = 0xcd;
        bytes[35] = 0xab;
        ImageIdentity identity;
        std::memcpy(identity.board.data(), "board", 6);
        std::memcpy(identity.version.data(), "1.2.3", 6);
        std::memcpy(bytes.data() + 48, identity.version.data(), 32);
        std::memcpy(bytes.data() + 80, "prusa_remote_display", 21);
        switch (scenario) {
        case 1:
            end_error = true;
            break;
        case 2:
            read_error = true;
            break;
        case 3:
            identity.magic[0] = 0;
            break; // legacy/no descriptor
        case 4:
            identity.board[0] = 'x';
            break;
        case 5:
            identity.format = 2;
            break;
        case 6:
            identity.min_bootloader_schema = 2;
            break;
        case 7:
            identity.version[4] = '1';
            bytes[52] = '1';
            break; // older signed image
        case 8:
            identity.reserved[0] = 1;
            break;
        case 9:
            identity.board.fill('a');
            break;
        case 10:
            bytes[28] = 0;
            bytes[29] = 1;
            break; // descriptor outside segment
        case 11:
            bytes[80] = 'x';
            break;
        case 12:
            bytes[52] = '4'; // standard/custom version mismatch
            break;
        case 13:
            identity.min_bootloader_schema = 0;
            break;
        case 14:
            identity.board[31] = 'x'; // noncanonical bytes after NUL
            break;
        default:
            break;
        }
        std::memcpy(bytes.data() + 288, &identity, sizeof(identity));
        const auto digest = sha(bytes.data(), bytes.size());
        const Candidate candidate{1, "board", "esp32s3", "1.2.3", 1, bytes.size(), digest};
        const DeviceCompatibility device{"board", "esp32s3", "1.2.2", 1, 0x200000};
        {
            ImageIngestion ingestion{device_ingestion_port()};
            CHECK(!ingestion.verify());
            CHECK(ingestion.begin(candidate, device, DowngradePolicy::Reject));
            CHECK(!ingestion.verify());
            CHECK(ingestion.append(bytes.data(), bytes.size()));
            CHECK(ingestion.finish());
            CHECK(ends == 0 && reads == 0);
            CHECK(ingestion.verify() == (scenario == 0));
            CHECK(ends == 1 && !live && aborts == 0);
            CHECK(reads == (scenario == 1 ? 0U : 1U));
            CHECK(!ingestion.verify());
            ingestion.cancel();
        }
        CHECK(ends == 1 && aborts == 0);
    }
}

void local_rejections() {
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        reset();
        std::uint64_t size = 3;
        switch (scenario) {
        case 0:
            missing_running = true;
            break;
        case 1:
            missing_target = true;
            break;
        case 2:
            target.type = 1;
            break;
        case 3:
            target.subtype = 0;
            break;
        case 4:
            target.subtype = 0x20;
            break;
        case 5:
            target.address = running.address;
            break;
        case 6:
            target.size = 2;
            break;
        default:
            size = 0;
            break;
        }
        CHECK(device_ingestion_port().begin(size) == IngestionBegin::FailedNeedsAbort);
        device_ingestion_port().abort();
        CHECK(begins == 0 && aborts == 0);
    }
    for (const auto size : {std::uint64_t{0xfffffffe}, std::uint64_t{0xffffffff},
                            std::numeric_limits<std::uint64_t>::max()}) {
        reset();
        CHECK(device_ingestion_port().begin(size) == IngestionBegin::FailedNeedsAbort);
        CHECK(begins == 0);
    }
    reset();
    begin_error = true;
    CHECK(device_ingestion_port().begin(3) == IngestionBegin::FailedNeedsAbort);
    device_ingestion_port().abort();
    CHECK(begins == 1 && aborts == 0); // No allocated handle on sequential begin failure.
    reset();
    start_error = true;
    CHECK(device_ingestion_port().begin(3) == IngestionBegin::FailedNeedsAbort);
    device_ingestion_port().abort();
    CHECK(begins == 0 && aborts == 0);
}

void failure_lifecycle() {
    const auto original = payload();
    const auto digest = sha(original.data(), original.size());
    const Candidate candidate{1, "board", "esp32s3", "1.2.3", 1, original.size(), digest};
    const DeviceCompatibility device{"board", "esp32s3", "1.2.2", 1, 0x200000};
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        reset();
        auto metadata = candidate;
        if (scenario == 0) {
            metadata.board = "wrong";
        }
        begin_error = scenario == 1;
        start_error = scenario == 2;
        ImageIngestion ingestion{device_ingestion_port()};
        CHECK(!ingestion.begin(metadata, device, DowngradePolicy::Reject));
        CHECK(begins == (scenario == 1 ? 1U : 0U));
        CHECK(aborts == 0 && !live);
    }
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        reset();
        {
            ImageIngestion ingestion{device_ingestion_port()};
            CHECK(ingestion.begin(candidate, device, DowngradePolicy::Reject));
            auto bytes = original;
            if (scenario == 0) {
                bytes.back() ^= 1;
            }
            write_error = scenario == 1;
            update_error = scenario == 2;
            finish_error = scenario == 3;
            const auto size = scenario == 4 ? bytes.size() - 1 : bytes.size();
            const bool appended = ingestion.append(bytes.data(), size);
            CHECK(appended == (scenario != 1 && scenario != 2));
            if (scenario == 5) {
                ingestion.cancel();
                CHECK(ingestion.status() == IngestionStatus::Cancelled);
            } else {
                CHECK(!ingestion.finish());
                constexpr IngestionStatus expected[] = {
                    IngestionStatus::DigestMismatch, IngestionStatus::WriteFailed,
                    IngestionStatus::WriteFailed, IngestionStatus::DigestFailed,
                    IngestionStatus::Truncated};
                CHECK(ingestion.status() == expected[scenario]);
            }
            CHECK(aborts == 1 && !live);
        }
        CHECK(aborts == 1);
    }
}

void refused_contender_preserves_incumbent() {
    reset();
    const auto bytes = payload();
    const auto digest = sha(bytes.data(), bytes.size());
    const Candidate candidate{1, "board", "esp32s3", "1.2.3", 1, bytes.size(), digest};
    const DeviceCompatibility device{"board", "esp32s3", "1.2.2", 1, 0x200000};
    {
        ImageIngestion incumbent{device_ingestion_port()};
        CHECK(incumbent.begin(candidate, device, DowngradePolicy::Reject));
        CHECK(incumbent.append(bytes.data(), 64));
        {
            ImageIngestion contender{device_ingestion_port()};
            CHECK(!contender.begin(candidate, device, DowngradePolicy::Reject));
            CHECK(contender.status() == IngestionStatus::BeginFailed);
            contender.cancel();
        }
        CHECK(live && aborts == 0 && begins == 1);
        CHECK(incumbent.append(bytes.data() + 64, bytes.size() - 64));
        CHECK(incumbent.finish());
        CHECK(incumbent.status() == IngestionStatus::DigestMatched);
        CHECK(stored == bytes);
    }
    CHECK(!live && aborts == 1);
}

void direct_guards() {
    reset();
    auto& port = device_ingestion_port();
    ImageDigest digest{};
    CHECK(!port.write(nullptr, 0));
    CHECK(!port.finish_digest(digest));
    CHECK(port.begin(1) == IngestionBegin::Started);
    CHECK(port.begin(1) == IngestionBegin::Refused);
    CHECK(!port.write(nullptr, 1));
    CHECK(!port.write(nullptr, 2));
    CHECK(!port.finish_digest(digest));
    CHECK(writes == 0);
    const std::uint8_t byte = 0xe9;
    CHECK(port.write(&byte, 1));
    CHECK(!port.write(&byte, 1));
    CHECK(port.finish_digest(digest));
    CHECK(!port.write(nullptr, 0));
    CHECK(!port.finish_digest(digest));
    port.abort();
    CHECK(aborts == 1);
}
struct Admission final : InstallationAdmission {
    bool held{false};
    bool activation_locked{false};
    bool acquire() override {
        if (held) {
            return false;
        }
        held = true;
        return true;
    }
    void release(bool retain_until_restart) override {
        activation_locked = activation_locked || retain_until_restart;
        if (!activation_locked) {
            held = false;
        }
    }
};

std::vector<std::uint8_t> identity_payload() {
    std::vector<std::uint8_t> bytes(376, 0);
    bytes[0] = 0xe9;
    bytes[1] = 1;
    bytes[28] = 0x58;
    bytes[29] = 1;
    bytes[32] = 0x32;
    bytes[33] = 0x54;
    bytes[34] = 0xcd;
    bytes[35] = 0xab;
    ImageIdentity identity;
    std::memcpy(identity.board.data(), "board", 6);
    std::memcpy(identity.version.data(), "1.2.3", 6);
    std::memcpy(bytes.data() + 48, identity.version.data(), 32);
    std::memcpy(bytes.data() + 80, "prusa_remote_display", 21);
    std::memcpy(bytes.data() + 288, &identity, sizeof(identity));
    return bytes;
}
std::string manifest_for(const std::vector<std::uint8_t>& bytes, bool newer = false) {
    return "{\"schemaVersion\":1,\"boardTarget\":\"board\",\"chipTarget\":\"esp32s3\","
           "\"productVersion\":\"" +
           std::string(newer ? "9.0.0" : "1.2.3") +
           "\",\"compatibility\":{\"minBootloaderSchema\":1},\"image\":{\"sizeBytes\":" +
           std::to_string(bytes.size()) + ",\"sha256\":\"" + sha(bytes.data(), bytes.size()) +
           "\"}}";
}

void reservation_and_stale_guards() {
    reset();
    auto& port = device_installation_port();
    const auto first = port.reserve();
    CHECK(first.valid());
    CHECK(!port.reserve().valid());
    CHECK(device_ingestion_port().begin(376) == IngestionBegin::Refused);
    port.release(first);
    const auto second = port.reserve();
    CHECK(second.valid() && !(second == first));
    CHECK(port.begin(second, 376) == IngestionBegin::Started);
    const auto before = aborts;
    port.abort(first);
    port.release(first);
    device_ingestion_port().abort();
    CHECK(live && aborts == before);
    ImageDigest digest{};
    ImageIdentity identity{};
    CHECK(port.begin(first, 376) == IngestionBegin::Refused);
    CHECK(!port.write(first, nullptr, 0));
    CHECK(!port.finish_digest(first, digest));
    CHECK(!port.finalize(first, identity));
    CHECK(port.select(first, {}) == SelectionResult::Rejected);
    CHECK(port.select(second, {}) == SelectionResult::Rejected);
    port.release(second);
    CHECK(!live && aborts == before + 1);
}

void concurrent_reservation() {
    reset();
    auto& port = device_installation_port();
    std::atomic<unsigned> ready{0};
    InstallationLease first;
    InstallationLease second;
    const auto run = [&port, &ready](InstallationLease& output) {
        ready.fetch_add(1);
        while (ready.load() != 2) {
            std::this_thread::yield();
        }
        output = port.reserve();
    };
    std::thread one{run, std::ref(first)};
    std::thread two{run, std::ref(second)};
    one.join();
    two.join();
    CHECK(first.valid() != second.valid());
    port.release(first.valid() ? first : second);
}

void installation_failures() {
    // Real pinned SHA; SDK signature and flash calls remain mocks. No claim of
    // valid signed images, actual RSA rejection or physical boot metadata.
    const DeviceCompatibility device{"board", "esp32s3", "1.2.2", 1, 0x200000};
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        reset();
        auto bytes = identity_payload();
        if (scenario == 1) {
            bytes[288 + 16] = 'x';
        }
        const auto input = manifest_for(bytes, scenario == 2);
        if (scenario == 3) {
            bytes.back() = 1;
        } // stream digest differs
        Admission admission;
        {
            Installation transaction{device_installation_port(), admission};
            CHECK(transaction.request(input, device, DowngradePolicy::Reject));
            CHECK(transaction.append(bytes.data(), bytes.size()));
            if (scenario == 3) {
                CHECK(!transaction.finish());
            } else {
                CHECK(transaction.finish());
                if (scenario == 0) {
                    end_error = true;
                }
                const bool verified = transaction.verify();
                CHECK(verified == (scenario >= 4));
                if (verified) {
                    CHECK(!live && ends == 1 && aborts == 0);
                    // Signature finalization has consumed the handle, but the
                    // independent reservation still blocks ALL other writers.
                    CHECK(!device_installation_port().reserve().valid());
                    CHECK(device_ingestion_port().begin(bytes.size()) == IngestionBegin::Refused);
                    device_ingestion_port().abort();
                    CHECK(aborts == 0);
                    if (scenario == 4) {
                        selection_error = true;
                        CHECK(!transaction.activate());
                    } else if (scenario == 5) {
                        target.address += 16;
                        CHECK(!transaction.activate());
                    } else if (scenario == 6) {
                        transaction.cancel();
                        CHECK(!transaction.activate());
                    } // scenario7: destructor after successful verification
                }
            }
        }
        CHECK(selections == (scenario == 4 ? 1U : 0U));
        CHECK(!admission.held && !admission.activation_locked);
        const auto fresh = device_installation_port().reserve();
        CHECK(fresh.valid());
        device_installation_port().release(fresh);
    }
}

void binding_and_selected_latch() {
    reset();
    const auto bytes = identity_payload();
    const auto digest_text = sha(bytes.data(), bytes.size());
    const Candidate approved{1, "board", "esp32s3", "1.2.3", 1, bytes.size(), digest_text};
    auto& port = device_installation_port();
    const auto lease = port.reserve();
    CHECK(port.begin(lease, bytes.size()) == IngestionBegin::Started);
    CHECK(port.write(lease, bytes.data(), bytes.size()));
    ImageDigest digest{};
    ImageIdentity identity{};
    CHECK(port.finish_digest(lease, digest));
    CHECK(port.finalize(lease, identity));
    auto changed = approved;
    changed.sha256 =
        std::string_view{"0000000000000000000000000000000000000000000000000000000000000000"};
    CHECK(port.select(lease, changed) == SelectionResult::Rejected);
    CHECK(selections == 0);
    changed = approved;
    changed.version = "9.0.0";
    CHECK(port.select(lease, changed) == SelectionResult::Rejected);
    CHECK(selections == 0);
    port.release(lease);

    reset();
    Admission admission;
    const DeviceCompatibility device{"board", "esp32s3", "1.2.2", 1, 0x200000};
    {
        Installation transaction{port, admission};
        CHECK(transaction.request(manifest_for(bytes), device, DowngradePolicy::Reject));
        CHECK(transaction.append(bytes.data(), bytes.size()));
        CHECK(transaction.finish());
        CHECK(transaction.verify());
        // The discovery result can change after verification. Selection must
        // use the reserved, authenticated target, never query a new candidate.
        missing_target = true;
        CHECK(transaction.activate());
        CHECK(selected_target == &target && selections == 1);
        transaction.cancel();
        CHECK(!transaction.activate());
    }
    CHECK(admission.held && admission.activation_locked);
    CHECK(!port.reserve().valid());
    CHECK(device_ingestion_port().begin(bytes.size()) == IngestionBegin::Refused);
    device_ingestion_port().abort();
    port.release(lease);
    port.abort(lease);
    CHECK(aborts == 0 && selections == 1 && selected_target == &target);
    // This singleton intentionally stays selected until process exit (reboot).
}

void uncertain_activation() {
    reset();
    const auto bytes = identity_payload();
    Admission admission;
    auto& port = device_installation_port();
    const DeviceCompatibility device{"board", "esp32s3", "1.2.2", 1, 0x200000};
    {
        Installation transaction{port, admission};
        CHECK(transaction.request(manifest_for(bytes), device, DowngradePolicy::Reject));
        CHECK(transaction.append(bytes.data(), bytes.size()));
        CHECK(transaction.finish());
        CHECK(transaction.verify());
        selection_write_error = true;
        CHECK(!transaction.activate());
        CHECK(transaction.status() == InstallationStatus::ActivationUncertain);
        CHECK(selected_target == &target && selections == 1);
        prd::app_model::OtaObservation observation;
        CHECK(transaction.observation(observation));
        CHECK(observation.phase == prd::app_model::OtaPhase::Failed);
        CHECK(!transaction.activate());
        transaction.cancel();
        CHECK(transaction.status() == InstallationStatus::ActivationUncertain);
    }
    CHECK(admission.held && admission.activation_locked);
    CHECK(!port.reserve().valid());
    CHECK(device_ingestion_port().begin(bytes.size()) == IngestionBegin::Refused);
    device_ingestion_port().abort();
    CHECK(aborts == 0 && selections == 1 && selected_target == &target);
    std::puts(
        "PASS: uncertain activation retains target and admission without success/reboot claim");
}
} // namespace

extern "C" int prd_test_sha_starts(mbedtls_sha256_context* context, int is224) {
    return start_error ? -1 : mbedtls_sha256_starts(context, is224);
}
extern "C" int prd_test_sha_update(mbedtls_sha256_context* context, const unsigned char* bytes,
                                   std::size_t size) {
    return update_error ? -1 : mbedtls_sha256_update(context, bytes, size);
}
extern "C" int prd_test_sha_finish(mbedtls_sha256_context* context, unsigned char* output) {
    return finish_error ? -1 : mbedtls_sha256_finish(context, output);
}

const esp_partition_t* esp_ota_get_running_partition() {
    return missing_running ? nullptr : &running;
}
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* from) {
    CHECK(from == nullptr);
    return missing_target ? nullptr : &target;
}
esp_err_t esp_ota_begin(const esp_partition_t* partition, std::size_t size,
                        esp_ota_handle_t* handle) {
    ++begins;
    CHECK(partition == &target && size == OTA_WITH_SEQUENTIAL_WRITES && !live);
    if (begin_error) {
        return -1;
    }
    live = true;
    *handle = 0; // Deliberately valid zero handle: never use it as an ownership sentinel.
    return ESP_OK;
}
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* bytes, std::size_t size) {
    ++writes;
    CHECK(live && handle == 0 && size > 0);
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    CHECK(!stored.empty() || data[0] == 0xe9);
    if (write_error) {
        return -1; // Includes sequential erase failure; session is still owned.
    }
    stored.insert(stored.end(), data, data + size);
    return ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle) {
    CHECK(live && handle == 0);
    ++aborts;
    live = false;
    return ESP_OK;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle) {
    CHECK(live && handle == 0);
    ++ends;
    live = false;
    return end_error ? -1 : ESP_OK;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition) {
    CHECK(!live && ends == 1 && reads == 1 && partition == &target);
    ++selections;
    if (selection_error) {
        return ESP_ERR_OTA_VALIDATE_FAILED; // Guaranteed before otadata writes in pinned IDF.
    }
    selected_target = partition;
    if (selection_write_error) {
        return -1;
    } // Model a reported error AFTER metadata changed.
    return ESP_OK;
}
esp_err_t esp_partition_read(const esp_partition_t* partition, std::size_t offset, void* output,
                             std::size_t size) {
    CHECK(!live && ends == 1 && partition == &target);
    ++reads;
    if (read_error || offset > stored.size() || size > stored.size() - offset) {
        return -1;
    }
    std::memcpy(output, stored.data() + offset, size);
    return ESP_OK;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--uncertain") {
        uncertain_activation();
        return 0;
    }
    known_vectors();
    successful_chunks();
    local_rejections();
    failure_lifecycle();
    refused_contender_preserves_incumbent();
    direct_guards();
    verified_handoff();
    reservation_and_stale_guards();
    concurrent_reservation();
    installation_failures();
    binding_and_selected_latch(); // selected singleton must be last
    std::puts(
        "PASS: SHA/legacy verification, installation ordering, binding, lease and selection latch");
}
