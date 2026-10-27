#pragma once

#include "wiknow/core.hpp"

namespace wiknow {
// Admission already happened at capture time. A producer may enqueue after the
// poll begins, so an earlier poll-clock snapshot must never filter this transfer.
template <typename Take>
bool retain_next_observation(RetainedObservation &pending, Observation &observation, Take take) {
    if (pending.pending() || !take(observation))
        return false;
    return pending.retain(observation);
}
} // namespace wiknow
