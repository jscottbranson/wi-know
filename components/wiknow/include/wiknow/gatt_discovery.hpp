#pragma once
#include "wiknow/core.hpp"

namespace wiknow {
// Fixed configured identities keep cooldowns even when unrelated advertisers flood
// the receiver. No address correlation, queue eviction, or heap allocation.
class GattSchedule {
    std::array<uint64_t, 16> attempted_{};
    std::array<bool, 16> used_{};
    const DeviceIdentity *targets_{};
    size_t count_{};
    uint32_t cooldown_{};
    uint64_t selected_at_{};
    Observation selected_{};
    bool pending_{};

  public:
    GattSchedule(const DeviceIdentity *targets, size_t count, uint32_t cooldown)
        : targets_(targets), count_(count <= 16 ? count : 0), cooldown_(cooldown) {}
    bool offer(const Observation &o, bool connectable_1m, uint64_t now, const Rule *rules,
               size_t rule_count) {
        if (pending_ || !connectable_1m || o.radio != Radio::ble || o.address_type > 1 ||
            (o.address_type == 1 && (o.address[0] & 0xc0) != 0xc0) ||
            !gatt_candidate(rules, rule_count, o))
            return false;
        for (size_t i = 0; i < count_; ++i) {
            const auto &t = targets_[i];
            if (t.radio != Radio::ble || t.address_type != o.address_type || t.address != o.address)
                continue;
            // Fail closed on a backwards clock until its previous value catches up.
            if (used_[i] && (now < attempted_[i] || now - attempted_[i] < cooldown_))
                return false;
            used_[i] = true;
            attempted_[i] = now;
            selected_ = o;
            selected_at_ = now;
            pending_ = true;
            return true;
        }
        return false;
    }
    bool take(uint64_t now, Observation &out) {
        if (!pending_)
            return false;
        pending_ = false;
        if (now < selected_at_ || now - selected_at_ > 1000)
            return false;
        out = selected_;
        return true;
    }
    void cancel() {
        pending_ = false;
    }
    bool pending() const {
        return pending_;
    }
};
// Host-queue-owned lifecycle. Tokens distinguish late callbacks even if NimBLE
// reuses a connection handle. Only a disconnect/cancel callback or host reset
// releases a session; a controller command ACK alone does not.
class GattLifecycle {
  public:
    enum class Phase { idle, connecting, discovering, stopping };

  private:
    Phase phase_{Phase::idle};
    uint32_t token_{};
    uint16_t handle_{0xffff};
    uint64_t deadline_{}, stopping_at_{};
    bool reset_requested_{};

  public:
    Phase phase() const {
        return phase_;
    }
    uint32_t token() const {
        return token_;
    }
    uint16_t handle() const {
        return handle_;
    }
    bool busy() const {
        return phase_ != Phase::idle;
    }
    bool current(uint32_t token) const {
        return busy() && token == token_;
    }
    bool expired(uint64_t now) const {
        return now >= deadline_;
    }
    bool begin(uint64_t now, uint32_t timeout) {
        if (busy())
            return false;
        if (++token_ == 0)
            ++token_;
        phase_ = Phase::connecting;
        deadline_ = now + timeout;
        reset_requested_ = false;
        return true;
    }
    bool connected(uint32_t token, uint16_t handle, uint64_t now, bool desired) {
        if (!current(token))
            return false;
        handle_ = handle;
        if (phase_ != Phase::connecting || expired(now) || !desired)
            return false;
        phase_ = Phase::discovering;
        return true;
    }
    bool service(uint32_t token, uint16_t handle) const {
        return current(token) && phase_ == Phase::discovering && handle == handle_;
    }
    void stop(uint64_t now) {
        if (!busy() || phase_ == Phase::stopping)
            return;
        phase_ = Phase::stopping;
        stopping_at_ = now;
    }
    bool request_reset(uint64_t now) {
        if (phase_ != Phase::stopping || reset_requested_ || now < stopping_at_ ||
            now - stopping_at_ < 2000)
            return false;
        reset_requested_ = true;
        return true;
    }
    void finish() {
        ++token_;
        phase_ = Phase::idle;
        handle_ = 0xffff;
    }
};
} // namespace wiknow
