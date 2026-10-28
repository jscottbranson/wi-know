#include "wiknow/capture_policy.hpp"
#include "wiknow/discovery.hpp"
#include <cassert>
#include <set>
using namespace wiknow;
static Identity hello(uint8_t id, uint32_t boot = 1) {
    Identity h;
    h.parent = {2, 3, 4, 5, 6, 7};
    h.worker = {2, 0, 0, 0, 0, id};
    h.parent_boot = 99;
    h.worker_boot = boot;
    h.nonce = 12;
    return h;
}
int main() {
    assert(hardware_identity(0, 16, 0) == HardwareIdentity::worker);
    assert(hardware_identity(16, 16, 0) == HardwareIdentity::parent);
    assert(hardware_identity(8, 16, 0) == HardwareIdentity::invalid);
    assert(hardware_identity(0, 16, 1) == HardwareIdentity::parent);
    assert(hardware_identity(16, 16, 2) == HardwareIdentity::worker);
    assert(hardware_identity(8, 16, 1) == HardwareIdentity::parent);
    assert(hardware_identity(0, 16, 3) == HardwareIdentity::invalid);
    // Real framing round trip, incompatible version, truncation and stale session.
    Packet packet{};
    Command command{};
    command.lease_ms = 10000;
    command.dwell_ms = 500;
    assert(pack_command(command, packet));
    auto id = hello(1);
    id.token = 42;
    assert(pack_identity(id, packet, MessageType::session_poll));
    uint8_t bytes[max_frame];
    auto length = encode_packet(packet, bytes, sizeof(bytes));
    Decoder decoder;
    Packet decoded;
    bool ready = false;
    for (size_t i = 0; i < length; ++i)
        ready = decoder.feed(bytes[i], decoded);
    assert(ready);
    Identity identity;
    Packet inner;
    assert(unpack_identity(decoded, identity, &inner) && same_session(id, identity));
    assert(unpack_command(inner, command));
    assert(!controller_conflict(decoded, id.parent, id.parent_boot));
    assert(controller_conflict(decoded, id.parent, id.parent_boot + 1));
    Packet manual{};
    assert(controller_conflict(manual, id.parent, id.parent_boot));
    Packet control{};
    pack_identity(id, control, MessageType::discover);
    auto foreign = id.parent;
    foreign[5]++;
    assert(controller_conflict(control, foreign, id.parent_boot));
    assert(!controller_conflict(control, id.parent, id.parent_boot));
    decoded.payload[0] = 2;
    assert(!unpack_identity(decoded, identity));
    decoded.size = 31;
    assert(!unpack_identity(decoded, identity));
    WorkerIdentity worker;
    auto probe = id;
    probe.worker = {};
    assert(worker.assign(id, 1, id.worker, 1, probe, 0, 10000));
    assert(worker.accept_sequence(100));
    assert(worker.assign(id, 1, id.worker, 1, probe, 10, 10000));
    assert(worker.expires == 10000); // duplicate ACK does not renew lease/reset sequence
    assert(!worker.accept_sequence(99));
    assert(!worker.accept_sequence(100));
    assert(worker.accept_sequence(101));
    assert(!worker.assign(id, 1, id.worker, 1, probe, 10000, 10000));
    auto expired_old = id;
    expired_old.token = 41;
    assert(!worker.assign(expired_old, 1, id.worker, 1, probe, 10000, 10000));
    assert(worker.accepts(id, 1, 9999));
    assert(!worker.accepts(id, 1, 10000));
    auto stale = id;
    stale.token = 41;
    assert(!worker.accepts(stale, 1, 1));
    assert(!worker.assign(stale, 1, id.worker, 1, probe, 1, 10000));
    stale = id;
    stale.worker_boot = 2;
    assert(!worker.accepts(stale, 1, 1));
    stale = id;
    stale.parent_boot = 98;
    assert(!worker.accepts(stale, 1, 1));
    stale = id;
    stale.nonce = 11;
    assert(!worker.assign(stale, 1, id.worker, 1, probe, 1, 10000));

    DiscoveryTable table(1, 3);
    uint32_t token = 100;
    auto *a = table.offer(hello(1), 0, 10000, ++token);
    assert(a && a->node == 1);
    table.roles(true, token);
    assert(a->identity.role == Role::child);
    assert(table.acknowledge(1, a->identity, 1, 10000));
    Membership members;
    members.seen(Node{1, 1, Role::child, true, true, true, 1}, 1);
    members = current_membership(members, table);
    assert(members.nodes()[0].online);
    members.expire(5001, 5000);
    members = current_membership(members, table);
    for (const auto &node : members.nodes())
        assert(!node.online); // no resurrection
    // Lost assignment ACK, new discovery nonce: a retried offer must follow the latest query.
    auto retry = a->identity;
    retry.nonce = 13;
    auto new_probe = retry;
    WorkerIdentity recovering;
    assert(recovering.assign(retry, 1, retry.worker, 1, new_probe, 2, 10000));
    auto *b = table.offer(hello(2), 1, 10000, ++token);
    assert(b && b->node == 2);
    table.roles(true, token);
    assert(b->identity.role == Role::follower);
    assert(!b->acknowledged);
    assert(table.acknowledge(2, b->identity, 2, 10000));
    auto *c = table.offer(hello(3), 2, 10000, ++token);
    assert(c);
    table.roles(true, token);
    assert(b->identity.role == Role::follower && c->identity.role == Role::child);
    assert(!table.offer(hello(4), 3, 10000, ++token));     // exhaustion
    assert(!table.offer(hello(1, 2), 3, 10000, ++token));  // duplicate identity / reboot waits
    assert(table.offer(hello(1), 3, 10000, ++token) == a); // retransmission never duplicates
    assert(table.acknowledge(1, a->identity, 9000, 10000));
    table.expire(11000);
    table.roles(true, token);
    assert(a->identity.role == Role::child && !table.get(2)->node && !table.get(3)->node);
    auto *replacement = table.offer(hello(4), 11001, 10000, ++token);
    assert(replacement->node == 2);
    table.roles(true, token);
    assert(replacement->identity.role == Role::follower);
    auto old = replacement->identity;
    table.expire(22000);
    auto *reboot = table.offer(hello(4, 2), 22001, 10000, ++token);
    assert(reboot);
    assert(!table.acknowledge(reboot->node, old, 22001, 10000));
    table.roles(false, token);
    assert(reboot->identity.role == Role::child);

    // Simultaneous boot: only the addressed slot may respond. Collisions cause
    // no assignment; fresh nonce changes response opportunities in later rounds.
    std::set<int> admitted;
    for (uint32_t nonce = 1; nonce <= 64 && admitted.size() < 16; ++nonce) {
        for (uint8_t slot = 0; slot < discovery_slots; ++slot) {
            int candidate = 0, count = 0;
            for (int i = 1; i <= 16; ++i)
                if (!admitted.count(i) &&
                    discovery_slot(hello(uint8_t(i)).worker, 1, nonce) == slot) {
                    candidate = i;
                    ++count;
                }
            if (count == 1)
                admitted.insert(candidate);
        }
    }
    assert(admitted.size() == 16);
    // Admission is a capture-time snapshot and remains valid when roles change.
    CapturePolicy policy{Role::child};
    policy.assignment.count = 1;
    policy.expires = 100;
    Observation observation;
    observation.radio = Radio::wifi;
    observation.at_ms = 99;
    assert(policy.accepts(observation));
    CapturePolicy changed{Role::follower};
    assert(!changed.accepts(observation));
    assert(policy.accepts(observation));
}
