#pragma once

#include "wiknow/discovery.hpp"

namespace wiknow {
// Keep the original deadline when stale/unrelated frames arrive. Controller
// detection must run before reply filtering, including on foreign addresses.
template <class Now, class Receive, class Conflict, class Matches>
bool receive_bus_reply(Packet &packet, uint32_t timeout, Now now, Receive receive,
                       Conflict conflict, Matches matches) {
    const auto until = now() + timeout;
    do {
        if (receive(packet, 1)) {
            if (conflict(packet))
                return false;
            if (matches(packet))
                return true;
        }
    } while (now() < until);
    return false;
}

// Validate and renew the live reservation before exposing a report to membership,
// coverage or observation delivery. A reply can arrive after its lease expired
// while the parent was transmitting or waiting for the worker.
inline bool accept_session_report(DiscoveryTable &table, const Packet &packet, uint16_t node,
                                  uint32_t sequence, uint64_t now, uint32_t lease, Report &report) {
    const auto *entry = table.get(node);
    if (!entry || !entry->node || !entry->acknowledged || packet.node != node ||
        packet.sequence != sequence || packet.type != MessageType::session_report)
        return false;
    Identity identity{};
    Packet inner{};
    Report decoded{};
    if (!unpack_identity(packet, identity, &inner) || !same_session(entry->identity, identity) ||
        packet.boot != identity.worker_boot || !unpack_report(inner, decoded) ||
        decoded.role != entry->identity.role || !table.acknowledge(node, identity, now, lease))
        return false;
    report = decoded;
    return true;
}
} // namespace wiknow
