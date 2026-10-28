#pragma once
#include "wiknow/core.hpp"
#include <algorithm>

namespace wiknow {
enum class HardwareIdentity { invalid, worker, parent };
inline HardwareIdentity hardware_identity(unsigned high_samples, unsigned samples,
                                          uint8_t override_role) {
    if (override_role > 2 || !samples || high_samples > samples)
        return HardwareIdentity::invalid;
    if (override_role)
        return override_role == 1 ? HardwareIdentity::parent : HardwareIdentity::worker;
    if (high_samples == samples)
        return HardwareIdentity::parent;
    return high_samples == 0 ? HardwareIdentity::worker : HardwareIdentity::invalid;
}
using DeviceId = std::array<uint8_t, 6>;
constexpr uint8_t discovery_version = 1, discovery_slots = 32;
constexpr size_t identity_size = 32;
constexpr uint8_t capability_wifi24 = 1, capability_wifi5 = 2, capability_follow = 4;
struct Identity {
    DeviceId parent{}, worker{};
    uint32_t parent_boot{}, worker_boot{}, token{}, nonce{};
    Role role{Role::child};
    uint8_t capabilities{7}, slot{};
};
inline bool valid_device(const DeviceId &id) {
    return std::any_of(id.begin(), id.end(), [](uint8_t b) { return b != 0; });
}
inline uint8_t discovery_slot(const DeviceId &id, uint32_t boot, uint32_t nonce) {
    uint32_t hash = 2166136261u ^ nonce;
    for (auto byte : id)
        hash = (hash ^ byte) * 16777619u;
    for (int i = 0; i < 4; ++i)
        hash = (hash ^ uint8_t(boot >> (8 * i))) * 16777619u;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    return uint8_t(hash % discovery_slots);
}
inline bool universal_type(MessageType type) {
    return uint8_t(type) >= 3 && uint8_t(type) <= 8;
}
inline bool pack_identity(const Identity &id, Packet &packet, MessageType type) {
    if (!universal_type(type) || packet.size + identity_size > packet.payload.size())
        return false;
    std::move_backward(packet.payload.begin(), packet.payload.begin() + packet.size,
                       packet.payload.begin() + packet.size + identity_size);
    auto *p = packet.payload.data();
    p[0] = discovery_version;
    p[1] = uint8_t(id.role);
    p[2] = id.capabilities;
    p[3] = id.slot;
    std::copy(id.parent.begin(), id.parent.end(), p + 4);
    std::copy(id.worker.begin(), id.worker.end(), p + 10);
    const uint32_t fields[] = {id.parent_boot, id.worker_boot, id.token, id.nonce};
    for (int f = 0; f < 4; ++f)
        for (int b = 0; b < 4; ++b)
            p[16 + f * 4 + b] = uint8_t(fields[f] >> (8 * b));
    packet.size += identity_size;
    packet.type = type;
    return true;
}
inline bool unpack_identity(const Packet &packet, Identity &id, Packet *inner = nullptr) {
    if (!universal_type(packet.type) || packet.size < identity_size)
        return false;
    const auto *p = packet.payload.data();
    if (p[0] != discovery_version || p[1] > uint8_t(Role::follower) || p[2] > 7 ||
        p[3] >= discovery_slots || (uint8_t(packet.type) <= 6 && packet.size != identity_size))
        return false;
    id.role = Role(p[1]);
    id.capabilities = p[2];
    id.slot = p[3];
    std::copy(p + 4, p + 10, id.parent.begin());
    std::copy(p + 10, p + 16, id.worker.begin());
    uint32_t *fields[] = {&id.parent_boot, &id.worker_boot, &id.token, &id.nonce};
    for (int f = 0; f < 4; ++f) {
        *fields[f] = 0;
        for (int b = 0; b < 4; ++b)
            *fields[f] |= uint32_t(p[16 + f * 4 + b]) << (8 * b);
    }
    if (!valid_device(id.parent))
        return false;
    if (inner) {
        *inner = packet;
        inner->size -= identity_size;
        std::copy(packet.payload.begin() + identity_size, packet.payload.begin() + packet.size,
                  inner->payload.begin());
        inner->type =
            packet.type == MessageType::session_poll ? MessageType::poll : MessageType::report;
    }
    return true;
}
inline bool controller_conflict(const Packet &packet, const DeviceId &self, uint32_t boot) {
    // A manual poll also proves an incompatible controller owns this bus.
    if (packet.type == MessageType::poll)
        return true;
    if (packet.type != MessageType::discover && packet.type != MessageType::assign &&
        packet.type != MessageType::session_poll)
        return false;
    Identity identity{};
    return unpack_identity(packet, identity) &&
           (identity.parent != self || identity.parent_boot != boot);
}
inline bool same_session(const Identity &a, const Identity &b) {
    return a.parent == b.parent && a.parent_boot == b.parent_boot && a.worker == b.worker &&
           a.worker_boot == b.worker_boot && a.token == b.token && a.role == b.role;
}
struct DiscoveredNode {
    Identity identity{};
    uint16_t node{};
    uint64_t expires{};
    bool acknowledged{};
};
class DiscoveryTable {
  public:
    DiscoveryTable(uint16_t first = 1, uint16_t last = max_nodes) : first_(first), last_(last) {}
    const std::array<DiscoveredNode, max_nodes> &nodes() const {
        return nodes_;
    }
    DiscoveredNode *get(uint16_t node) {
        return node >= first_ && node <= last_ && node <= max_nodes ? &nodes_[node - 1] : nullptr;
    }
    void expire(uint64_t now) {
        for (auto &n : nodes_)
            if (n.node && now >= n.expires)
                n = {};
    }
    DiscoveredNode *offer(const Identity &hello, uint64_t now, uint32_t lease, uint32_t token) {
        if (!valid_device(hello.worker) || !(hello.capabilities & capability_wifi24) || !token)
            return nullptr;
        expire(now);
        for (auto &n : nodes_)
            if (n.node && n.identity.worker == hello.worker) {
                // An identity with another live boot is ambiguous; wait for its lease.
                if (n.identity.worker_boot != hello.worker_boot)
                    return nullptr;
                return &n;
            }
        for (uint16_t id = first_; id <= last_ && id <= max_nodes; ++id) {
            auto &n = nodes_[id - 1];
            if (!n.node) {
                n.identity = hello;
                n.identity.token = token;
                n.identity.role = Role::child;
                n.node = id;
                n.expires = now + lease;
                return &n;
            }
        }
        return nullptr;
    }
    bool acknowledge(uint16_t node, const Identity &identity, uint64_t now, uint32_t lease) {
        auto *n = get(node);
        if (!n || !n->node || now >= n->expires || !same_session(n->identity, identity))
            return false;
        n->acknowledged = true;
        n->expires = now + lease;
        return true;
    }
    // At two workers reserve one follower; a single survivor always scans.
    // Role transitions get a fresh token and must be acknowledged again.
    void roles(bool enabled, uint32_t &token) {
        size_t count = 0;
        uint16_t follower = 0, existing = 0;
        for (const auto &n : nodes_)
            if (n.node) {
                ++count;
                if (n.identity.capabilities & capability_follow) {
                    follower = n.node;
                    if (n.identity.role == Role::follower)
                        existing = n.node;
                }
            }
        if (existing)
            follower = existing;
        if (!enabled || count < 2)
            follower = 0;
        for (auto &n : nodes_)
            if (n.node) {
                Role desired = n.node == follower ? Role::follower : Role::child;
                if (n.identity.role != desired) {
                    n.identity.role = desired;
                    n.identity.token = ++token;
                    n.acknowledged = false;
                }
            }
    }

  private:
    uint16_t first_, last_;
    std::array<DiscoveredNode, max_nodes> nodes_{};
};
inline Membership current_membership(const Membership &previous, DiscoveryTable &table) {
    Membership current;
    for (const auto &known : previous.nodes()) {
        auto *entry = table.get(known.id);
        if (known.online && entry && entry->node && entry->acknowledged &&
            entry->identity.worker_boot == known.boot && entry->identity.role == known.role)
            current.seen(known, known.last_seen);
    }
    return current;
}
class WorkerIdentity {
  public:
    Identity identity{};
    uint16_t node{};
    uint64_t expires{};
    bool active(uint64_t now) const {
        return node && now < expires;
    }
    bool assign(const Identity &offer, uint16_t address, const DeviceId &self, uint32_t boot,
                const Identity &probe, uint64_t now, uint32_t lease) {
        if (!address || address > max_nodes || offer.worker != self || offer.worker_boot != boot ||
            offer.parent != probe.parent || offer.parent_boot != probe.parent_boot ||
            offer.nonce != probe.nonce || !offer.token || offer.role == Role::parent)
            return false;
        if (node && offer.parent == identity.parent && offer.parent_boot == identity.parent_boot) {
            if (offer.token == identity.token)
                return active(now) && address == node && same_session(identity, offer);
            if (!sequence_newer(offer.token, identity.token))
                return false;
        }
        identity = offer;
        node = address;
        expires = now + lease;
        have_sequence_ = false;
        return true;
    }
    bool accept_sequence(uint32_t sequence) {
        if (have_sequence_ && !sequence_newer(sequence, last_sequence_))
            return false;
        have_sequence_ = true;
        last_sequence_ = sequence;
        return true;
    }
    bool accepts(const Identity &command, uint16_t address, uint64_t now) const {
        return active(now) && address == node && same_session(identity, command);
    }

  private:
    bool have_sequence_{};
    uint32_t last_sequence_{};
};
} // namespace wiknow
