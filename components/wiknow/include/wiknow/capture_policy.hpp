#pragma once

#include "wiknow/core.hpp"

namespace wiknow {
// Snapshot at reception, before deferred Wi-Fi parsing. Later reassignment must
// not change whether an already captured observation belongs to this worker.
struct CapturePolicy {
    Role role{Role::parent};
    Command assignment{};
    uint64_t expires{};
    uint16_t node{};

    bool accepts(const Observation &observation) const {
        if (role == Role::parent)
            return true;
        if (observation.at_ms >= expires)
            return false;
        if (role == Role::child)
            return assignment.count && observation.radio == Radio::wifi;
        return assignment.follow && observation.radio == assignment.target_radio &&
               observation.address_type == assignment.target_address_type &&
               observation.address == assignment.target;
    }
};
} // namespace wiknow
