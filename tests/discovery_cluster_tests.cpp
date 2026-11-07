#include "wiknow/bus_exchange.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>

using namespace wiknow;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #condition);            \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

// Exercise the same wire envelope, state machines and retained delivery at both
// ends. Physical UART scheduling and electrical collisions require hardware.
static Packet wire(const Packet &packet) {
    std::array<uint8_t, max_frame> bytes{};
    const auto size = encode_packet(packet, bytes.data(), bytes.size());
    CHECK(size);
    Decoder decoder;
    Packet decoded{};
    for (size_t i = 0; i < size; ++i)
        CHECK(decoder.feed(bytes[i], decoded) == (i + 1 == size));
    return decoded;
}
struct Worker {
    DeviceId device{};
    uint32_t boot{};
    Identity probe{};
    WorkerIdentity session{};
    RetainedObservation retained{};
    unsigned deliveries{};
};
struct Cluster {
    static constexpr uint32_t lease = 10000;
    std::array<Worker, 5> workers{};
    DiscoveryTable table{1, 5};
    std::array<ReportReceipt, max_nodes> receipts{};
    uint32_t token{100}, sequence{};
    uint64_t now{};
    unsigned collisions{}, dropped_acks{}, rejected_old_commands{}, duplicate_reports{};
    uint16_t silent_address{};
    bool lose_first_ack{};

    Cluster() {
        for (size_t i = 0; i < workers.size(); ++i) {
            workers[i].device = {2, 0, 0, 0, 0, uint8_t(i + 1)};
            workers[i].boot = uint32_t(i + 1);
            Observation observation{};
            observation.boot = workers[i].boot;
            observation.sequence = 1;
            observation.rssi = -50;
            CHECK(workers[i].retained.retain(observation));
        }
    }
    Worker &worker(const DeviceId &device) {
        for (auto &w : workers)
            if (w.device == device)
                return w;
        CHECK(false);
        return workers[0];
    }
    void discover(uint32_t nonce, uint8_t slot) {
        table.expire(now);
        table.roles(true, token);
        Identity query{};
        query.parent = {2, 3, 4, 5, 6, 7};
        query.parent_boot = 99;
        query.nonce = nonce;
        query.slot = slot;
        Packet request{};
        CHECK(pack_identity(query, request, MessageType::discover));
        const auto decoded = wire(request);
        unsigned responders = 0;
        Packet response{};
        for (auto &w : workers) {
            CHECK(unpack_identity(decoded, w.probe));
            if (!w.session.active(now) && discovery_slot(w.device, w.boot, nonce) == slot) {
                auto hello = w.probe;
                hello.worker = w.device;
                hello.worker_boot = w.boot;
                response = {};
                CHECK(pack_identity(hello, response, MessageType::hello));
                ++responders;
            }
        }
        if (responders > 1)
            ++collisions; // Simultaneous replies are lost, with no partial admission.
        if (responders == 1) {
            Identity hello{};
            CHECK(unpack_identity(wire(response), hello));
            CHECK(table.offer(hello, now, lease, next_discovery_token(token)));
            table.roles(true, token);
        }
        if (auto *entry = table.next_offer()) {
            entry->identity.nonce = nonce;
            entry->identity.slot = slot;
            Packet offer{};
            offer.node = entry->node;
            CHECK(pack_identity(entry->identity, offer, MessageType::assign));
            const auto received = wire(offer);
            Identity identity{};
            CHECK(unpack_identity(received, identity));
            auto &w = worker(identity.worker);
            if (w.session.assign(identity, received.node, w.device, w.boot, w.probe, now, lease)) {
                Packet ack{};
                ack.node = received.node;
                CHECK(pack_identity(identity, ack, MessageType::assigned));
                Identity acknowledged{};
                const auto decoded_ack = wire(ack);
                CHECK(unpack_identity(decoded_ack, acknowledged));
                if (entry->node == silent_address || (lose_first_ack && !dropped_acks))
                    ++dropped_acks;
                else
                    CHECK(table.acknowledge(decoded_ack.node, acknowledged, now, lease));
            }
        }
    }
    void poll(bool lose_report = false, bool omit_receipt = false) {
        for (const auto &entry : table.nodes()) {
            if (!entry.node || !entry.acknowledged)
                continue;
            auto &w = worker(entry.identity.worker);
            Command command{};
            if (!omit_receipt)
                command.ack = receipts[entry.node - 1].acknowledgement();
            Packet request{};
            request.node = entry.node;
            request.boot = 99;
            request.sequence = ++sequence;
            CHECK(pack_command(command, request));
            CHECK(pack_identity(entry.identity, request, MessageType::session_poll));
            const auto received = wire(request);
            Identity identity{};
            Packet inner{};
            CHECK(unpack_identity(received, identity, &inner));
            CHECK(w.session.accepts(identity, received.node, now));
            CHECK(w.session.accept_sequence(received.sequence));
            CHECK(unpack_command(inner, command));
            w.session.expires = now + lease;
            w.retained.acknowledge(command.ack);
            // A duplicated/delayed command must neither execute nor consume data.
            CHECK(!w.session.accept_sequence(received.sequence));
            CHECK(!w.session.accept_sequence(received.sequence - 1));
            ++rejected_old_commands;
            Report report{};
            report.role = w.session.identity.role;
            w.retained.report(report, now);
            Packet response{};
            response.node = w.session.node;
            response.boot = w.boot;
            response.sequence = received.sequence;
            CHECK(pack_report(report, response));
            CHECK(pack_identity(w.session.identity, response, MessageType::session_report));
            const auto decoded = wire(response);
            if (lose_report)
                continue;
            CHECK(accept_session_report(table, decoded, request.node, request.sequence, now, lease,
                                        report));
            CHECK(report.role == entry.identity.role);
            if (report.has_observation) {
                if (receipts[entry.node - 1].accept(report.observation))
                    ++w.deliveries;
                else
                    ++duplicate_reports;
            }
        }
    }
    unsigned acknowledged() const {
        unsigned count = 0;
        for (const auto &entry : table.nodes())
            count += entry.node && entry.acknowledged;
        return count;
    }
    void run(unsigned sweeps) {
        // Nonce 2 puts two of these factory identities in the same slot; the
        // next sweep separates them and must admit both without duplicate IDs.
        for (uint32_t nonce = 2; nonce < sweeps + 2; ++nonce)
            for (uint8_t slot = 0; slot < discovery_slots; ++slot) {
                discover(nonce, slot);
                // Periodic response loss and withheld receipts exercise both
                // retention and parent-side duplicate report suppression.
                poll(slot % 7 == 0, slot % 2 == 1);
                now += 100;
            }
    }
};
int main() {
    Cluster normal;
    normal.lose_first_ack = true;
    normal.run(5);
    CHECK(normal.acknowledged() == 5);
    CHECK(normal.collisions > 0);
    CHECK(normal.duplicate_reports > 0);
    CHECK(normal.dropped_acks == 1 && normal.rejected_old_commands > 0);
    unsigned followers = 0;
    for (const auto &entry : normal.table.nodes())
        followers += entry.node && entry.identity.role == Role::follower;
    CHECK(followers == 1);
    normal.poll();
    for (const auto &w : normal.workers)
        CHECK(w.deliveries == 1 && !w.retained.pending());

    // A permanently lost ACK at a low address must not prevent later workers
    // joining. Keep below the lease expiry so expiry cannot hide starvation.
    Cluster failed;
    failed.silent_address = 1;
    failed.run(2);
    CHECK(failed.now < Cluster::lease);
    CHECK(failed.dropped_acks > 1);
    CHECK(failed.acknowledged() == 4);
    for (const auto &entry : failed.table.nodes())
        CHECK(!entry.node || entry.node == 1 || entry.acknowledged);
    std::puts("Five-worker discovery and session delivery tests passed");
}
