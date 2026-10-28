#include "wiknow/ble_reassembly.hpp"
#include "wiknow/remote_id.hpp"
#include <cassert>
#include <cstring>
#include <vector>
using namespace wiknow;
using Bytes = std::vector<uint8_t>;
static Bytes rid() {
    Bytes bytes{30, 0x16, 0xfa, 0xff, 0x0d, 7, 0x02, 0x12};
    bytes.resize(31);
    std::memcpy(bytes.data() + 8, "ASSEMBLY-TEST", 13);
    return bytes;
}
static BleFragment fragment(const Bytes &bytes, size_t offset, size_t size, BleDataStatus status,
                            uint64_t at = 100) {
    BleFragment f{};
    f.key.address = {2, 1, 2, 3, 4, 5};
    f.key.sid = 1;
    f.key.primary_phy = 1;
    f.key.secondary_phy = 1;
    f.capture.at_ms = at;
    f.capture.rssi = -42;
    f.capture.policy.role = Role::parent;
    f.capture.policy.node = 7;
    f.status = status;
    f.data = bytes.data() + offset;
    f.size = size;
    return f;
}
static Observation observation(const BleAdvertisement &report) {
    Observation o{};
    assert(parse_ble(report.data.data(), report.size, o));
    o.address = report.key.address;
    o.address_type = report.key.address_type;
    o.rssi = report.capture.rssi;
    o.at_ms = report.capture.at_ms;
    o.node = report.capture.policy.node;
    o.timing_uncertainty_ms = report.duration_ms;
    return o;
}
int main() {
    const auto bytes = rid();
    BleAdvertisement output;
    // All split positions, including the AD length/type/UUID header, on both
    // PHY paths. Preserve exact bytes, first capture time and assignment policy.
    for (uint8_t phy : {1, 3}) {
        for (size_t split = 1; split < bytes.size(); ++split) {
            BleAssembler a;
            auto first = fragment(bytes, 0, split, BleDataStatus::incomplete);
            first.key.primary_phy = first.key.secondary_phy = phy;
            auto last = fragment(bytes, split, bytes.size() - split, BleDataStatus::complete, 120);
            last.key = first.key;
            last.capture.policy.node = 9;
            last.capture.rssi = -80;
            assert(a.accept(first, output) == BleAssemblyResult::pending);
            assert(a.accept(last, output) == BleAssemblyResult::complete);
            assert(output.size == bytes.size() &&
                   std::equal(bytes.begin(), bytes.end(), output.data.begin()));
            assert(output.capture.at_ms == 100 && output.duration_ms == 20);
            assert(output.capture.policy.node == 7 && output.capture.rssi == -42);
            auto o = observation(output);
            assert(remote_id_messages(o).count == 1);
            char json[log_line_capacity];
            assert(drone_detection_json(o, 0, {}, Gps{}, json, sizeof(json)));
            assert(std::strstr(json, "ASSEMBLY-TEST"));
            Report report{};
            report.has_observation = true;
            report.observation = o;
            Packet packet{};
            packet.node = o.node;
            assert(pack_report(report, packet));
            Report decoded{};
            assert(unpack_report(packet, decoded));
            assert(decoded.observation.payload == o.payload &&
                   decoded.observation.payload_size == o.payload_size);
            assert(a.stats().assemblies == 1 && a.stats().complete_reports == 0);
        }
    }
    // Interleaved addresses, SIDs and scan responses must never share chains.
    {
        BleAssembler a;
        std::array<BleFragment, 4> first;
        for (size_t i = 0; i < first.size(); ++i) {
            first[i] = fragment(bytes, 0, 10, BleDataStatus::incomplete);
            if (i == 1)
                first[i].key.address[5] = 6;
            if (i == 2)
                first[i].key.sid = 2;
            if (i == 3)
                first[i].key.properties = 0x08;
            assert(a.accept(first[i], output) == BleAssemblyResult::pending);
        }
        for (size_t i = first.size(); i--;) {
            auto last = fragment(bytes, 10, bytes.size() - 10, BleDataStatus::complete, 110);
            last.key = first[i].key;
            assert(a.accept(last, output) == BleAssemblyResult::complete);
            assert(output.key.address == first[i].key.address &&
                   output.key.sid == first[i].key.sid &&
                   output.key.properties == first[i].key.properties);
            assert(observation(output).payload_size == bytes.size());
        }
        assert(a.stats().assemblies == 4);
    }
    // Complete-only/legacy reports and repeated valid observations remain usable.
    {
        BleAssembler a;
        auto complete = fragment(bytes, 0, bytes.size(), BleDataStatus::complete);
        assert(a.accept(complete, output) == BleAssemblyResult::complete);
        assert(a.accept(complete, output) == BleAssemblyResult::complete);
        complete.key.properties = 0x10;
        complete.key.sid = 0xff;
        complete.key.secondary_phy = 0;
        assert(a.accept(complete, output) == BleAssemblyResult::complete);
        assert(a.stats().complete_reports == 3 && a.stats().assemblies == 0);
        // Equal fragment bytes can be legitimate separate AD structures. Never
        // guess that the controller repeated or pre-accumulated a report.
        Bytes repeated{2, 9, 'A', 2, 9, 'A'};
        assert(a.accept(fragment(repeated, 0, 3, BleDataStatus::incomplete), output) ==
               BleAssemblyResult::pending);
        assert(a.accept(fragment(repeated, 3, 3, BleDataStatus::complete), output) ==
               BleAssemblyResult::complete);
        assert(output.size == repeated.size() &&
               observation(output).payload_size == repeated.size());
    }
    // Truncation is terminal. A later independent complete event is not joined.
    {
        BleAssembler a;
        assert(a.accept(fragment(bytes, 0, 5, BleDataStatus::incomplete), output) ==
               BleAssemblyResult::pending);
        assert(a.accept(fragment(bytes, 5, 3, BleDataStatus::truncated), output) ==
               BleAssemblyResult::rejected);
        assert(a.accept(fragment(bytes, 0, bytes.size(), BleDataStatus::complete), output) ==
               BleAssemblyResult::complete);
        assert(output.size == bytes.size() && a.stats().truncations == 1);
    }
    // Byte and fragment bounds poison unfinished chains until their terminal;
    // even a tail that happens to contain valid AD/RID is not a standalone report.
    {
        BleAssembler a;
        Bytes long_data(256, 'x');
        assert(a.accept(fragment(long_data, 0, 200, BleDataStatus::incomplete), output) ==
               BleAssemblyResult::pending);
        assert(a.accept(fragment(long_data, 200, 56, BleDataStatus::incomplete), output) ==
               BleAssemblyResult::rejected);
        assert(a.accept(fragment(bytes, 0, bytes.size(), BleDataStatus::complete), output) ==
               BleAssemblyResult::rejected);
        assert(a.accept(fragment(bytes, 0, bytes.size(), BleDataStatus::complete), output) ==
               BleAssemblyResult::complete);
        assert(a.stats().overflows == 1);
        assert(a.accept(fragment(long_data, 0, 256, BleDataStatus::complete), output) ==
               BleAssemblyResult::rejected);
        assert(a.accept(fragment(long_data, 0, 256, BleDataStatus::incomplete), output) ==
               BleAssemblyResult::rejected);
        assert(a.accept(fragment(bytes, 0, bytes.size(), BleDataStatus::complete), output) ==
               BleAssemblyResult::rejected);
        assert(a.stats().overflows == 3);
        for (size_t i = 0; i < BleAssembler::fragment_limit; ++i)
            assert(a.accept(fragment(bytes, i, 1, BleDataStatus::incomplete), output) ==
                   BleAssemblyResult::pending);
        assert(a.accept(fragment(bytes, 8, 1, BleDataStatus::complete), output) ==
               BleAssemblyResult::rejected);
        assert(a.stats().overflows == 4);
        for (size_t i = 0; i < BleAssembler::fragment_limit - 1; ++i)
            assert(a.accept(fragment(bytes, i, 1, BleDataStatus::incomplete), output) ==
                   BleAssemblyResult::pending);
        assert(a.accept(fragment(bytes, 7, 1, BleDataStatus::complete), output) ==
               BleAssemblyResult::complete);
        assert(output.size == 8);
        assert(a.accept(fragment(long_data, 0, 200, BleDataStatus::incomplete), output) ==
               BleAssemblyResult::pending);
        assert(a.accept(fragment(long_data, 200, 55, BleDataStatus::complete), output) ==
               BleAssemblyResult::complete);
        assert(output.size == max_payload);
    }
    // No eviction makes late tails indistinguishable from complete-only reports.
    // Saturation quarantines everything until confirmed scan cancellation/reset.
    {
        BleAssembler a;
        for (size_t i = 0; i < BleAssembler::capacity; ++i) {
            auto first = fragment(bytes, 0, 1, BleDataStatus::incomplete);
            first.key.sid = uint8_t(i);
            assert(a.accept(first, output) == BleAssemblyResult::pending);
        }
        auto extra = fragment(bytes, 0, 1, BleDataStatus::incomplete);
        extra.key.sid = 9;
        assert(a.accept(extra, output) == BleAssemblyResult::rejected);
        assert(a.needs_restart() && a.stats().capacity_resets == 1);
        for (size_t i = 0; i <= BleAssembler::capacity; ++i) {
            auto tail = fragment(bytes, 0, bytes.size(), BleDataStatus::complete);
            tail.key.sid = uint8_t(i);
            assert(a.accept(tail, output) == BleAssemblyResult::rejected);
        }
        a.reset();
        assert(a.stats().cancelled == BleAssembler::capacity && !a.needs_restart());
        assert(a.accept(fragment(bytes, 0, bytes.size(), BleDataStatus::complete), output) ==
               BleAssemblyResult::complete);
    }
    // Time bounds do not renew on each fragment; missing terminals cannot leave
    // stale state to concatenate with a later independent advertising event.
    {
        BleAssembler a;
        assert(a.accept(fragment(bytes, 0, 1, BleDataStatus::incomplete), output) ==
               BleAssemblyResult::pending);
        assert(a.accept(fragment(bytes, 1, 1, BleDataStatus::incomplete, 349), output) ==
               BleAssemblyResult::pending);
        a.expire(350);
        assert(a.needs_restart() && a.stats().timeouts == 1);
        assert(a.accept(fragment(bytes, 0, bytes.size(), BleDataStatus::complete, 351), output) ==
               BleAssemblyResult::rejected);
        a.reset();
        assert(a.accept(fragment(bytes, 0, bytes.size(), BleDataStatus::complete, 360), output) ==
               BleAssemblyResult::complete);
        assert(output.size == bytes.size());
        assert(a.accept(fragment(bytes, 0, 1, BleDataStatus::incomplete, 400), output) ==
               BleAssemblyResult::pending);
        a.reset(); // Host reset or standalone Wi-Fi/BLE boundary.
        assert(a.accept(fragment(bytes, 0, bytes.size(), BleDataStatus::complete, 500), output) ==
               BleAssemblyResult::complete);
        assert(output.size == bytes.size() && a.stats().cancelled == 2);
    }
    // Inconsistent properties/PHY/target/interval poison only that identified
    // chain. Address type and SID are identity components, never normalized away.
    for (unsigned change = 0; change < 5; ++change) {
        BleAssembler a;
        auto first = fragment(bytes, 0, 1, BleDataStatus::incomplete);
        first.key.properties = 4;
        assert(a.accept(first, output) == BleAssemblyResult::pending);
        auto next = fragment(bytes, 1, 1, BleDataStatus::incomplete);
        next.key = first.key;
        if (change == 0)
            next.key.properties ^= 1;
        if (change == 1)
            next.key.primary_phy = 3;
        if (change == 2)
            next.key.secondary_phy = 3;
        if (change == 3)
            next.key.direct_address[0] = 1;
        if (change == 4)
            next.key.periodic_interval = 1;
        assert(a.accept(next, output) == BleAssemblyResult::rejected);
        auto terminal = fragment(bytes, 0, bytes.size(), BleDataStatus::complete);
        terminal.key = first.key;
        assert(a.accept(terminal, output) == BleAssemblyResult::rejected);
        assert(a.accept(terminal, output) == BleAssemblyResult::complete);
    }
    {
        BleAssembler a;
        auto first = fragment(bytes, 0, 1, BleDataStatus::incomplete);
        assert(a.accept(first, output) == BleAssemblyResult::pending);
        auto different = fragment(bytes, 0, bytes.size(), BleDataStatus::complete);
        different.key.address_type = 1;
        assert(a.accept(different, output) == BleAssemblyResult::complete);
        assert(output.key.address_type == 1 && a.stats().assemblies == 0);
        auto rest = fragment(bytes, 1, bytes.size() - 1, BleDataStatus::complete);
        assert(a.accept(rest, output) == BleAssemblyResult::complete);
        assert(output.key.address_type == 0 && a.stats().assemblies == 1);
    }
    for (unsigned invalid = 0; invalid < 6; ++invalid) {
        BleAssembler a;
        auto bad = fragment(bytes, 0, bytes.size(), BleDataStatus::complete);
        if (invalid == 0)
            bad.key.address_type = 0xff;
        if (invalid == 1)
            bad.key.sid = 0xff;
        if (invalid == 2)
            bad.key.properties = 0x20;
        if (invalid == 3)
            bad.key.primary_phy = 2;
        if (invalid == 4)
            bad.key.secondary_phy = 4;
        if (invalid == 5)
            bad.data = nullptr;
        assert(a.accept(bad, output) == BleAssemblyResult::rejected);
        assert(a.stats().ambiguous == 1);
    }
    // Missing bytes are not synthesized: malformed assembled AD is rejected by
    // the downstream parser. Invisible upstream loss at an AD boundary cannot
    // be distinguished with HCI metadata; the documented profile makes no claim otherwise.
    {
        BleAssembler a;
        assert(a.accept(fragment(bytes, 0, 3, BleDataStatus::incomplete), output) ==
               BleAssemblyResult::pending);
        assert(a.accept(fragment(bytes, 10, bytes.size() - 10, BleDataStatus::complete), output) ==
               BleAssemblyResult::complete);
        Observation o{};
        assert(!parse_ble(output.data.data(), output.size, o));
        auto empty = fragment(bytes, 0, 0, BleDataStatus::incomplete);
        assert(a.accept(empty, output) == BleAssemblyResult::rejected);
        assert(a.accept(fragment(bytes, 0, bytes.size(), BleDataStatus::complete), output) ==
               BleAssemblyResult::rejected);
    }
}
