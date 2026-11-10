#include "wiknow/bus_exchange.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace wiknow;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #condition);            \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

struct Arrival {
    uint64_t at;
    Packet packet;
};
struct Bus {
    uint64_t now{};
    size_t next{};
    std::vector<Arrival> arrivals;
    bool receive(Packet &packet, uint32_t timeout) {
        now += timeout;
        if (next == arrivals.size() || arrivals[next].at > now)
            return false;
        packet = arrivals[next++].packet;
        return true;
    }
};

static Identity worker_identity() {
    Identity identity{};
    identity.parent = {1, 2, 3, 4, 5, 6};
    identity.worker = {6, 5, 4, 3, 2, 1};
    identity.parent_boot = 10;
    identity.worker_boot = 20;
    identity.token = 30;
    return identity;
}
static Packet identity_packet(const Identity &identity, MessageType type) {
    Packet packet{};
    packet.node = 9; // A worker must also notice controllers addressing other workers.
    CHECK(pack_identity(identity, packet, type));
    return packet;
}
static void worker_conflicts() {
    const auto own = worker_identity();
    auto foreign = own;
    ++foreign.parent[0];
    for (auto type : {MessageType::discover, MessageType::assign, MessageType::session_poll}) {
        auto packet = identity_packet(foreign, type);
        CHECK(worker_controller_conflict(packet, own.parent, true));
        CHECK(!worker_controller_conflict(packet, own.parent, false));
        packet.payload[0] = discovery_version + 1;
        CHECK(!worker_controller_conflict(packet, own.parent, true));
        auto rebooted = own;
        ++rebooted.parent_boot;
        CHECK(!worker_controller_conflict(identity_packet(rebooted, type), own.parent, true));
    }
    for (auto type : {MessageType::hello, MessageType::assigned, MessageType::session_report})
        CHECK(!worker_controller_conflict(identity_packet(foreign, type), own.parent, true));
    Packet manual{};
    CHECK(worker_controller_conflict(manual, own.parent, true));
    CHECK(worker_controller_conflict(manual, own.parent, false));
}
static void response_filtering() {
    Packet expected{};
    expected.type = MessageType::session_report;
    expected.node = 2;
    expected.sequence = 9;
    auto stale = expected;
    --stale.sequence;
    auto other = expected;
    ++other.node;
    auto matches = [&](const Packet &p) {
        return p.type == expected.type && p.node == expected.node &&
               p.sequence == expected.sequence;
    };
    auto no_conflict = [](const Packet &) { return false; };
    Packet out{};
    Bus bus{0, 0, {{1, stale}, {2, other}, {3, expected}}};
    CHECK(receive_bus_reply(
        out, 5, [&] { return bus.now; }, [&](Packet &p, uint32_t t) { return bus.receive(p, t); },
        no_conflict, matches));
    CHECK(bus.now == 3 && bus.next == 3 && out.sequence == 9);

    // Unrelated traffic cannot reset the deadline and extend bus ownership.
    bus = {0, 0, {{1, stale}, {2, stale}, {3, stale}, {4, other}, {5, other}, {6, expected}}};
    CHECK(!receive_bus_reply(
        out, 5, [&] { return bus.now; }, [&](Packet &p, uint32_t t) { return bus.receive(p, t); },
        no_conflict, matches));
    CHECK(bus.now == 5 && bus.next == 5);

    auto identity = worker_identity();
    auto foreign = identity;
    ++foreign.parent[0];
    bus = {0, 0, {{1, stale}, {2, identity_packet(foreign, MessageType::discover)}, {3, expected}}};
    bool halted = false;
    CHECK(!receive_bus_reply(
        out, 5, [&] { return bus.now; }, [&](Packet &p, uint32_t t) { return bus.receive(p, t); },
        [&](const Packet &p) {
            halted = controller_conflict(p, identity.parent, identity.parent_boot);
            return halted;
        },
        matches));
    CHECK(halted && bus.now == 2 && bus.next == 2);

    // A caller preempted past the deadline still accepts a reply that arrived in
    // time, but zero-timeout draining never waits for later traffic.
    bus = {0, 0, {{1, stale}, {2, expected}}};
    bool preempted = false;
    auto slow = [&](Packet &p, uint32_t t) {
        if (!preempted) {
            preempted = true;
            bus.now = 20; // other tasks ran
        }
        return bus.receive(p, t);
    };
    CHECK(receive_bus_reply(out, 5, [&] { return bus.now; }, slow, no_conflict, matches));
    CHECK(bus.now == 21 && bus.next == 2 && out.sequence == 9);
    bus = {0, 0, {{1, stale}, {30, expected}}};
    preempted = false;
    CHECK(!receive_bus_reply(out, 5, [&] { return bus.now; }, slow, no_conflict, matches));
    CHECK(bus.now == 21 && bus.next == 1); // drained only data present at resume
    bus = {0, 0, {{1, stale}, {2, identity_packet(foreign, MessageType::discover)}, {3, expected}}};
    preempted = false;
    halted = false;
    CHECK(!receive_bus_reply(
        out, 1, [&] { return bus.now; }, slow,
        [&](const Packet &p) {
            halted = controller_conflict(p, identity.parent, identity.parent_boot);
            return halted;
        },
        matches));
    CHECK(halted && bus.next == 2); // conflicts still stop draining
}
static void report_lease() {
    DiscoveryTable table(1, 1);
    const auto identity = worker_identity();
    auto *entry = table.offer(identity, 0, 100, identity.token);
    CHECK(entry && table.acknowledge(1, identity, 0, 100));
    Report report{};
    report.role = Role::child;
    report.dropped = 7;
    Packet packet{};
    packet.node = 1;
    packet.boot = identity.worker_boot;
    packet.sequence = 8;
    CHECK(pack_report(report, packet));
    CHECK(pack_identity(identity, packet, MessageType::session_report));
    Report out{};
    out.dropped = 99;
    CHECK(!accept_session_report(table, packet, 1, 8, 100, 100, out));
    CHECK(entry->expires == 100 && out.dropped == 99);
    CHECK(!accept_session_report(table, packet, 1, 8, 101, 100, out));
    CHECK(entry->expires == 100 && out.dropped == 99);
    CHECK(!accept_session_report(table, packet, 1, 9, 99, 100, out));
    CHECK(entry->expires == 100);
    auto wrong_boot = packet;
    ++wrong_boot.boot;
    CHECK(!accept_session_report(table, wrong_boot, 1, 8, 99, 100, out));
    CHECK(entry->expires == 100);
    entry->acknowledged = false;
    CHECK(!accept_session_report(table, packet, 1, 8, 99, 100, out));
    entry->acknowledged = true;
    CHECK(accept_session_report(table, packet, 1, 8, 99, 100, out));
    CHECK(entry->expires == 199 && out.dropped == 7);

    // A stale reply must neither renew the lease nor prevent the current reply
    // from being accepted in the same receive window.
    auto stale = packet;
    --stale.sequence;
    Bus bus{100, 0, {{101, stale}, {103, packet}}};
    Packet received{};
    CHECK(receive_bus_reply(
        received, 5, [&] { return bus.now; },
        [&](Packet &p, uint32_t t) { return bus.receive(p, t); },
        [](const Packet &) { return false; },
        [&](const Packet &p) {
            if (p.sequence != 8)
                CHECK(entry->expires == 199);
            return accept_session_report(table, p, 1, 8, bus.now, 100, out);
        }));
    CHECK(bus.now == 103 && entry->expires == 203);
}
int main() {
    worker_conflicts();
    response_filtering();
    report_lease();
    std::puts("Bus exchange tests passed");
}
