#pragma once
#include "wiknow/capture_policy.hpp"
#include <algorithm>

namespace wiknow {
// Values are the pinned NimBLE/HCI report constants, independent of SDK headers.
enum class BleDataStatus : uint8_t { complete = 0, incomplete = 1, truncated = 2 };
struct BleReportKey {
    std::array<uint8_t, 6> address{}; // Normalized byte order, not HCI little endian.
    uint8_t address_type{}, sid{}, properties{}, primary_phy{1}, secondary_phy{};
    std::array<uint8_t, 6> direct_address{};
    uint8_t direct_address_type{};
    uint16_t periodic_interval{};
};
struct BleCapture {
    CapturePolicy policy{};
    uint64_t at_ms{};
    int8_t rssi{};
};
struct BleFragment {
    BleReportKey key{};
    BleCapture capture{};
    BleDataStatus status{BleDataStatus::complete};
    const uint8_t *data{};
    size_t size{};
};
struct BleAdvertisement {
    BleReportKey key{};
    BleCapture capture{}; // First fragment's identity, policy, RSSI and timestamp.
    uint32_t duration_ms{};
    uint16_t size{};
    std::array<uint8_t, max_payload> data{};
};
struct BleAssemblyStats {
    uint32_t complete_reports{}, assemblies{}, timeouts{}, truncations{}, overflows{}, ambiguous{},
        capacity_resets{}, cancelled{};
};
enum class BleAssemblyResult { pending, complete, rejected };
// One serialized owner, fixed memory. No AD parsing, allocations, speculative
// duplicate suppression, or reuse of a discarded chain's COMPLETE tail.
class BleAssembler {
  public:
    static constexpr size_t capacity = 4, fragment_limit = 8;
    static constexpr uint64_t timeout_ms = 250;
    const BleAssemblyStats &stats() const {
        return stats_;
    }
    bool needs_restart() const {
        return quarantine_;
    }
    // Call only at a confirmed controller stop/reset/start boundary. A plain
    // application timeout is not sufficient to distinguish a late terminal tail.
    void reset() {
        for (auto &entry : entries_) {
            if (entry.used)
                ++stats_.cancelled;
            entry = {};
        }
        quarantine_ = false;
    }
    void expire(uint64_t now) {
        if (quarantine_)
            return;
        for (const auto &entry : entries_) {
            if (entry.used && (now < entry.report.capture.at_ms ||
                               now - entry.report.capture.at_ms >= timeout_ms)) {
                ++stats_.timeouts;
                quarantine_ = true; // No eviction: wait for the controller boundary.
            }
        }
    }
    BleAssemblyResult accept(const BleFragment &fragment, BleAdvertisement &output) {
        expire(fragment.capture.at_ms);
        if (quarantine_) {
            ++stats_.ambiguous;
            return BleAssemblyResult::rejected;
        }
        const auto &key = fragment.key;
        const bool legacy = key.properties & 0x10;
        // Anonymous/reserved addresses cannot be associated reliably.
        if (key.address_type > 3) {
            ++stats_.ambiguous;
            return BleAssemblyResult::rejected;
        }
        Entry *entry = nullptr, *free = nullptr;
        bool related = false;
        for (auto &candidate : entries_) {
            related = related || (candidate.used && candidate.report.key.address == key.address &&
                                  candidate.report.key.address_type == key.address_type);
            if (candidate.used && same_identity(candidate.report.key, key))
                entry = &candidate;
            if (!candidate.used)
                free = &candidate;
        }
        if (fragment.status == BleDataStatus::truncated) {
            if (!legacy && key.sid > 15 && related) {
                quarantine_ = true; // Cannot identify which same-address chain terminated.
                ++stats_.ambiguous;
            }
            if (entry)
                *entry = {};
            ++stats_.truncations;
            return BleAssemblyResult::rejected;
        }
        const bool terminal = fragment.status == BleDataStatus::complete;
        // HCI permits unavailable SID (0xff) on a complete nonlegacy report.
        // It is safe standalone only if no same-advertiser chain could own it.
        if (!legacy && key.sid > 15 && !(key.sid == 0xff && terminal && !related)) {
            ++stats_.ambiguous;
            if (!terminal || related)
                quarantine_ = true; // A later COMPLETE must not escape lost SID history.
            return BleAssemblyResult::rejected;
        }
        const bool valid =
            (key.properties & ~uint8_t(0x1f)) == 0 &&
            (key.primary_phy == 1 || key.primary_phy == 3) && key.secondary_phy <= 3 &&
            (terminal || fragment.status == BleDataStatus::incomplete) && (!legacy || terminal) &&
            (fragment.data || !fragment.size) && (terminal || fragment.size != 0);
        if (entry && entry->poisoned) {
            if (terminal)
                *entry = {};
            ++stats_.ambiguous;
            return BleAssemblyResult::rejected;
        }
        if (!valid || (entry && !consistent(entry->report.key, key))) {
            ++stats_.ambiguous;
            if (entry) {
                entry->poisoned = true;
                if (terminal)
                    *entry = {};
            } else if (!terminal) {
                if (!free)
                    quarantine();
                else {
                    free->used = free->poisoned = true;
                    free->report.key = key;
                    free->report.capture = fragment.capture;
                }
            }
            return BleAssemblyResult::rejected;
        }
        if (fragment.size > max_payload ||
            (entry && (fragment.size > max_payload - entry->report.size ||
                       entry->fragments >= fragment_limit))) {
            ++stats_.overflows;
            if (entry) {
                entry->poisoned = true;
                if (terminal)
                    *entry = {};
            } else if (!terminal) {
                if (!free)
                    quarantine();
                else {
                    free->used = free->poisoned = true;
                    free->report.key = key;
                    free->report.capture = fragment.capture;
                }
            }
            return BleAssemblyResult::rejected;
        }
        if (!entry && terminal) {
            output = {};
            output.key = key;
            output.capture = fragment.capture;
            output.size = uint16_t(fragment.size);
            if (fragment.size)
                std::copy_n(fragment.data, fragment.size, output.data.begin());
            ++stats_.complete_reports;
            return BleAssemblyResult::complete;
        }
        if (!entry) {
            if (!free) {
                quarantine();
                return BleAssemblyResult::rejected;
            }
            entry = free;
            entry->used = true;
            entry->report.key = key;
            entry->report.capture = fragment.capture;
        }
        if (fragment.size)
            std::copy_n(fragment.data, fragment.size,
                        entry->report.data.begin() + entry->report.size);
        entry->report.size += uint16_t(fragment.size);
        ++entry->fragments;
        if (!terminal)
            return BleAssemblyResult::pending;
        entry->report.duration_ms = uint32_t(fragment.capture.at_ms - entry->report.capture.at_ms);
        output = entry->report;
        *entry = {};
        ++stats_.assemblies;
        return BleAssemblyResult::complete;
    }

  private:
    struct Entry {
        BleAdvertisement report{};
        uint8_t fragments{};
        bool used{}, poisoned{};
    };
    std::array<Entry, capacity> entries_{};
    BleAssemblyStats stats_{};
    bool quarantine_{};
    void quarantine() {
        quarantine_ = true;
        ++stats_.capacity_resets;
    }
    static bool same_identity(const BleReportKey &a, const BleReportKey &b) {
        return a.address == b.address && a.address_type == b.address_type && a.sid == b.sid &&
               ((a.properties ^ b.properties) & 0x08) == 0; // Scan response is a separate chain.
    }
    static bool consistent(const BleReportKey &a, const BleReportKey &b) {
        return a.properties == b.properties && a.primary_phy == b.primary_phy &&
               a.secondary_phy == b.secondary_phy && a.periodic_interval == b.periodic_interval &&
               (!(a.properties & 0x04) || (a.direct_address == b.direct_address &&
                                           a.direct_address_type == b.direct_address_type));
    }
};
} // namespace wiknow
