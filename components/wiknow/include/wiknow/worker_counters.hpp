#pragma once

#include "wiknow/core.hpp"

namespace wiknow {
struct WorkerLoss {
    bool seen{}, online{};
    uint32_t boot{}, reported{};
    uint64_t total{};
};

// Parent-lifetime loss totals survive worker expiry and worker reboots. Reports
// are accepted only for the current poll, so same-boot decreases indicate wrap.
// As with the worker's uint32_t counter, multiple wraps between reports cannot
// be reconstructed.
class WorkerLossCounters {
  public:
    bool update(uint16_t node, uint32_t boot, uint32_t reported) {
        if (!node || node > max_nodes)
            return false;
        auto &worker = workers_[node - 1];
        bool changed =
            !worker.seen || !worker.online || worker.boot != boot || worker.reported != reported;
        uint32_t added =
            !worker.seen || worker.boot != boot ? reported : reported - worker.reported;
        worker.total += added;
        worker.seen = worker.online = true;
        worker.boot = boot;
        worker.reported = reported;
        return changed;
    }
    bool offline(uint16_t node) {
        if (!node || node > max_nodes || !workers_[node - 1].online)
            return false;
        workers_[node - 1].online = false;
        return true;
    }
    const WorkerLoss *get(uint16_t node) const {
        return node && node <= max_nodes && workers_[node - 1].seen ? &workers_[node - 1] : nullptr;
    }
    uint64_t total() const {
        uint64_t result = 0;
        for (const auto &worker : workers_)
            result += worker.total;
        return result;
    }

  private:
    std::array<WorkerLoss, max_nodes> workers_{};
};
} // namespace wiknow
