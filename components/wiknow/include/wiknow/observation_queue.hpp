#pragma once

#include "wiknow/core.hpp"

namespace wiknow {
// Callers synchronize access. Separate storage prevents ordinary traffic from
// using the capacity reserved for matching observations.
class ObservationQueue {
  public:
    static constexpr size_t ordinary_capacity = 32, matching_capacity = 16;
    bool push(const Observation &observation, bool matching) {
        return matching ? matching_.push(observation) : ordinary_.push(observation);
    }
    bool pop(Observation &observation) {
        return matching_.pop(observation) || ordinary_.pop(observation);
    }

  private:
    template <size_t Capacity> struct Ring {
        std::array<Observation, Capacity> entries{};
        size_t head{}, count{};
        bool push(const Observation &observation) {
            if (count == Capacity)
                return false;
            entries[(head + count) % Capacity] = observation;
            ++count;
            return true;
        }
        bool pop(Observation &observation) {
            if (!count)
                return false;
            observation = entries[head];
            head = (head + 1) % Capacity;
            --count;
            return true;
        }
    };
    Ring<ordinary_capacity> ordinary_{};
    Ring<matching_capacity> matching_{};
};
} // namespace wiknow
