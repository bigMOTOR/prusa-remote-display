// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "storage/key_value.hpp"

// An in-memory storage::KeyValue for host tests.
//
// This is what makes the whole of `config` testable: the record format, the §13.3 staged
// write, the previous-known-good fallback and the §13.4 migration dispatch all run here
// exactly as they run on the board, because `config` has no ESP-IDF adapter to bypass.
//
// It can be made to fail on demand, which is the point — the interesting paths in a staged
// write are the ones where a step does not complete, and those cannot be reached by a
// store that always works.

namespace prd::test {

class FakeKeyValue final : public storage::KeyValue {
  public:
    // Fail the next write to this exact (namespace, key). Cleared once it fires, so a test
    // can break one step of a staged write and let the rest proceed.
    void fail_write(const storage::Namespace space, const std::string& key) {
        write_failures_.insert(compose(space, key));
    }

    void fail_read(const storage::Namespace space, const std::string& key) {
        read_failures_.insert(compose(space, key));
    }

    // Fail the next erase of this exact (namespace, key). Cleared once it fires.
    //
    // ⬅️ ADDED FOR MILESTONE 3.10. Nothing erased anything before it: ruling R10 left the product
    // no erase at all until user-consented factory reset arrived. Now there is one, and its
    // interesting paths are the ones where a step does not complete — a reset that half-finished
    // must leave its journal in place and resume, and that is only reachable by breaking a step.
    void fail_erase(const storage::Namespace space, const std::string& key) {
        erase_failures_.insert(compose(space, key));
    }

    // Replace a stored value without going through write_blob — how a test simulates bit
    // rot, a torn write or a record from another firmware.
    void poke(const storage::Namespace space, const std::string& key,
              std::vector<std::uint8_t> value) {
        blobs_[compose(space, key)] = std::move(value);
    }

    // Remove a key as though it had never been written.
    //
    // NOT AN ERASE THE PRODUCT CAN PERFORM — nothing in the firmware deletes a key (R10).
    // It exists so a test can reach the store state a device is left in when a write never
    // happened, which is otherwise only reachable by failing that exact write during a
    // save. The activation-boundary tests need both routes to the same state.
    void forget(const storage::Namespace space, const std::string& key) {
        const auto composed = compose(space, key);
        blobs_.erase(composed);
        words_.erase(composed);
    }

    [[nodiscard]] bool has(const storage::Namespace space, const std::string& key) const {
        const auto composed = compose(space, key);
        return blobs_.count(composed) != 0 || words_.count(composed) != 0;
    }

    [[nodiscard]] std::vector<std::uint8_t> peek(const storage::Namespace space,
                                                 const std::string& key) const {
        const auto found = blobs_.find(compose(space, key));
        return found == blobs_.end() ? std::vector<std::uint8_t>{} : found->second;
    }

    [[nodiscard]] std::size_t erase_key_calls() const {
        return erase_key_calls_;
    }

    [[nodiscard]] storage::Result write_blob(const storage::Namespace space,
                                             const std::string_view key,
                                             const std::uint8_t* const data,
                                             const std::size_t length) override {
        if (!storage::key_is_storable(key)) {
            return storage::Result::InvalidKey;
        }

        const auto composed = compose(space, std::string{key});

        if (take(write_failures_, composed)) {
            return storage::Result::Failed;
        }

        blobs_[composed].assign(data, data + length);
        return storage::Result::Ok;
    }

    [[nodiscard]] storage::Result read_blob(const storage::Namespace space,
                                            const std::string_view key, std::uint8_t* const buffer,
                                            const std::size_t capacity,
                                            std::size_t& length_out) override {
        length_out = 0;

        if (!storage::key_is_storable(key)) {
            return storage::Result::InvalidKey;
        }

        const auto composed = compose(space, std::string{key});

        if (take(read_failures_, composed)) {
            return storage::Result::Failed;
        }

        const auto found = blobs_.find(composed);
        if (found == blobs_.end()) {
            return storage::Result::NotFound;
        }

        length_out = found->second.size();

        if (found->second.size() > capacity) {
            return storage::Result::TooLarge;
        }

        for (std::size_t index = 0; index < found->second.size(); ++index) {
            buffer[index] = found->second[index];
        }

        return storage::Result::Ok;
    }

    [[nodiscard]] storage::Result write_u32(const storage::Namespace space,
                                            const std::string_view key,
                                            const std::uint32_t value) override {
        if (!storage::key_is_storable(key)) {
            return storage::Result::InvalidKey;
        }

        const auto composed = compose(space, std::string{key});

        if (take(write_failures_, composed)) {
            return storage::Result::Failed;
        }

        words_[composed] = value;
        return storage::Result::Ok;
    }

    [[nodiscard]] storage::Result read_u32(const storage::Namespace space,
                                           const std::string_view key,
                                           std::uint32_t& value_out) override {
        if (!storage::key_is_storable(key)) {
            return storage::Result::InvalidKey;
        }

        const auto composed = compose(space, std::string{key});

        if (take(read_failures_, composed)) {
            return storage::Result::Failed;
        }

        const auto found = words_.find(composed);
        if (found == words_.end()) {
            return storage::Result::NotFound;
        }

        value_out = found->second;
        return storage::Result::Ok;
    }

    [[nodiscard]] storage::Result erase_key(const storage::Namespace space,
                                            const std::string_view key) override {
        ++erase_key_calls_;

        if (!storage::key_is_storable(key)) {
            return storage::Result::InvalidKey;
        }

        const auto composed = compose(space, std::string{key});

        if (take(erase_failures_, composed)) {
            return storage::Result::Failed;
        }

        const bool removed = blobs_.erase(composed) != 0 || words_.erase(composed) != 0;

        return removed ? storage::Result::Ok : storage::Result::NotFound;
    }

  private:
    [[nodiscard]] static std::string compose(const storage::Namespace space,
                                             const std::string& key) {
        return std::string{storage::name(space)} + "/" + key;
    }

    [[nodiscard]] static bool take(std::multiset<std::string>& failures, const std::string& key) {
        const auto found = failures.find(key);
        if (found == failures.end()) {
            return false;
        }

        failures.erase(found);
        return true;
    }

    std::multiset<std::string> erase_failures_;
    std::map<std::string, std::vector<std::uint8_t>> blobs_;
    std::map<std::string, std::uint32_t> words_;
    std::multiset<std::string> write_failures_;
    std::multiset<std::string> read_failures_;
    std::size_t erase_key_calls_{0};
};

} // namespace prd::test
