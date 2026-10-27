#pragma once

#include "wiknow/core.hpp"

namespace wiknow {
inline bool same_assignment(const Command &a, const Command &b) {
    if (a.count > max_channels || b.count > max_channels)
        return false;
    if (a.count != b.count || a.dwell_ms != b.dwell_ms || a.lease_ms != b.lease_ms ||
        a.follow != b.follow || a.target_radio != b.target_radio ||
        a.target_channel != b.target_channel || a.target_address_type != b.target_address_type ||
        a.target != b.target)
        return false;
    for (size_t i = 0; i < a.count; ++i)
        if (!(a.channels[i] == b.channels[i]))
            return false;
    return true;
}
inline const char *scan_state_name(ScanState state) {
    switch (state) {
    case ScanState::idle:
        return "idle";
    case ScanState::scanning:
        return "scanning";
    case ScanState::error:
        return "error";
    case ScanState::expired:
        return "expired";
    default:
        return "awaiting";
    }
}
class WorkerScanProgress {
  public:
    // Retry failures on the next accepted poll; ordinary lease renewals retain
    // the channels already visited. A new parent session always resets evidence.
    bool apply(const Command &command, bool parent_changed) {
        bool reset = !initialized_ || parent_changed ||
                     command.assignment_id != command_.assignment_id ||
                     !same_assignment(command_, command) ||
                     (status_.state == ScanState::error && error_reported_) ||
                     status_.state == ScanState::expired;
        command_ = command;
        initialized_ = true;
        if (reset) {
            error_reported_ = false;
            status_ = {};
            status_.assignment_id = command.assignment_id;
            status_.state = command.count || (command.follow && command.target_radio == Radio::wifi)
                                ? ScanState::awaiting
                                : ScanState::idle;
        }
        return reset;
    }
    void tuned(uint8_t channel) {
        status_.state = ScanState::scanning;
        status_.failed_channel = 0;
        for (size_t i = 0; i < command_.count && i < max_channels; ++i)
            if (command_.channels[i].number == channel)
                status_.confirmed_channels |= uint32_t(1) << i;
    }
    void failed(uint8_t channel) {
        error_reported_ = false;
        status_.state = ScanState::error;
        status_.failed_channel = channel;
        status_.confirmed_channels = 0;
    }
    void expire() {
        status_.state = ScanState::expired;
        status_.confirmed_channels = 0;
        status_.failed_channel = 0;
    }
    ScanStatus report() {
        // A dwell failure between polls must be sent once before a retry can
        // overwrite it with a successful first channel.
        if (status_.state == ScanState::error)
            error_reported_ = true;
        return status_;
    }
    const ScanStatus &status() const {
        return status_;
    }

  private:
    bool initialized_{}, error_reported_{};
    Command command_{};
    ScanStatus status_{};
};
struct Coverage {
    size_t planned{}, confirmed{}, errors{};
};
struct WorkerScanReport {
    bool desired_valid{}, received{};
    Command desired{};
    uint32_t boot{};
    ScanStatus scan{};
    uint64_t received_at{};
};
class ScanCoverage {
  public:
    void prepare(uint16_t node, Command &command) {
        if (!node || node > max_nodes)
            return;
        auto &entry = entries_[node - 1];
        if (!entry.desired_valid || !same_assignment(entry.desired, command)) {
            command.assignment_id = entry.desired.assignment_id + 1;
            entry.desired = command;
            entry.desired_valid = true;
            entry.received = false;
        } else
            command.assignment_id = entry.desired.assignment_id;
    }
    bool observe(uint16_t node, uint32_t boot, ScanStatus scan, uint64_t now) {
        if (!node || node > max_nodes)
            return false;
        auto &entry = entries_[node - 1];
        if (!entry.desired_valid || scan.assignment_id != entry.desired.assignment_id)
            return false;
        const uint32_t allowed =
            entry.desired.count == 32 ? UINT32_MAX : (uint32_t(1) << entry.desired.count) - 1;
        if (scan.confirmed_channels & ~allowed)
            return false;
        bool changed = !entry.received || entry.boot != boot || entry.scan.state != scan.state ||
                       entry.scan.confirmed_channels != scan.confirmed_channels ||
                       entry.scan.failed_channel != scan.failed_channel;
        entry.received = true;
        entry.boot = boot;
        entry.scan = scan;
        entry.received_at = now;
        return changed;
    }
    void offline(uint16_t node) {
        if (node && node <= max_nodes)
            entries_[node - 1].received = false;
    }
    const WorkerScanReport *get(uint16_t node) const {
        return node && node <= max_nodes ? &entries_[node - 1] : nullptr;
    }
    Coverage coverage(const Plan &plan, uint64_t now, uint32_t max_age) const {
        Coverage result{};
        for (size_t i = 0; i < plan.count; ++i) {
            const auto &assignment = plan.assignments[i];
            result.planned += assignment.count;
            const auto *entry = get(assignment.node);
            if (!entry || !entry->received || now < entry->received_at ||
                now - entry->received_at >= max_age || entry->desired.count != assignment.count)
                continue;
            bool current = true;
            for (size_t c = 0; c < assignment.count; ++c)
                if (!(entry->desired.channels[c] == assignment.channels[c]))
                    current = false;
            if (!current)
                continue;
            if (entry->scan.state == ScanState::error)
                ++result.errors;
            if (entry->scan.state == ScanState::scanning)
                for (size_t c = 0; c < assignment.count; ++c)
                    if (entry->scan.confirmed_channels & (uint32_t(1) << c))
                        ++result.confirmed;
        }
        return result;
    }

  private:
    std::array<WorkerScanReport, max_nodes> entries_{};
};
} // namespace wiknow
