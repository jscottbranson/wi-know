#include "wiknow/scan_status.hpp"
#include <cstdio>
#include <cstdlib>
using namespace wiknow;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #x);                                        \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
int main() {
    Membership membership;
    membership.seen(Node{1, 10, Role::child, true, true}, 0);
    Channel channels[] = {{1}, {36}};
    auto plan = membership.plan(channels, 2);
    Command command;
    command.count = 2;
    command.channels[0] = channels[0];
    command.channels[1] = channels[1];
    ScanCoverage parent;
    parent.prepare(1, command);
    CHECK(command.assignment_id == 1);
    CHECK(parent.coverage(plan, 0, 100).planned == 2);
    CHECK(parent.coverage(plan, 0, 100).confirmed == 0);
    WorkerScanProgress worker;
    CHECK(worker.apply(command, true));
    CHECK(worker.status().state == ScanState::awaiting);
    worker.tuned(1);
    CHECK(parent.observe(1, 10, worker.report(), 10));
    CHECK(parent.coverage(plan, 11, 100).confirmed == 1);
    command.ack = {true, 10, 5};
    parent.prepare(1, command);
    CHECK(command.assignment_id == 1);
    CHECK(!worker.apply(command, false));
    worker.tuned(36);
    CHECK(parent.observe(1, 10, worker.report(), 20));
    CHECK(parent.coverage(plan, 21, 100).confirmed == 2);
    CHECK(parent.coverage(plan, 120, 100).confirmed == 0);
    worker.failed(1);
    CHECK(!worker.apply(command, false)); // do not hide a failure between polls
    CHECK(worker.status().state == ScanState::error);
    CHECK(parent.observe(1, 10, worker.report(), 30));
    auto coverage = parent.coverage(plan, 31, 100);
    CHECK(coverage.confirmed == 0 && coverage.errors == 1);
    CHECK(worker.apply(command, false));
    worker.tuned(1);
    CHECK(parent.observe(1, 10, worker.report(), 40));
    CHECK(parent.coverage(plan, 41, 100).confirmed == 1);
    worker.expire();
    CHECK(parent.observe(1, 10, worker.report(), 50));
    CHECK(parent.coverage(plan, 51, 100).confirmed == 0);
    CHECK(worker.apply(command, false));
    worker.tuned(36);
    CHECK(parent.observe(1, 10, worker.report(), 60));
    CHECK(parent.coverage(plan, 61, 100).confirmed == 1);
    // Worker reboot resets visited channels even with the same generation.
    WorkerScanProgress rebooted;
    CHECK(rebooted.apply(command, true));
    CHECK(parent.observe(1, 11, rebooted.status(), 70));
    CHECK(parent.coverage(plan, 71, 100).confirmed == 0);
    rebooted.tuned(1);
    parent.observe(1, 11, rebooted.status(), 72);
    // A changed logical plan invalidates old evidence before that node is polled.
    auto changed_plan = plan;
    changed_plan.assignments[0].channels[0] = {6};
    CHECK(parent.coverage(changed_plan, 73, 100).confirmed == 0);
    command.channels[0] = {6};
    parent.prepare(1, command);
    CHECK(command.assignment_id == 2);
    CHECK(!parent.observe(1, 11, rebooted.status(), 74));
    CHECK(parent.coverage(changed_plan, 75, 100).confirmed == 0);
    CHECK(rebooted.apply(command, false));
    rebooted.tuned(6);
    parent.observe(1, 11, rebooted.status(), 76);
    CHECK(parent.coverage(changed_plan, 77, 100).confirmed == 1);
    parent.offline(1);
    CHECK(parent.coverage(changed_plan, 78, 100).confirmed == 0);
    // New parent session invalidates worker evidence, independent of generation.
    CHECK(rebooted.apply(command, true));
    CHECK(rebooted.status().confirmed_channels == 0);
    // A single dual-band worker must cover the entire 36-channel list.
    Channel all[] = {{1},   {2},   {3},   {4},   {5},   {6},   {7},   {8},   {9},
                     {10},  {11},  {36},  {40},  {44},  {48},  {52},  {56},  {60},
                     {64},  {100}, {104}, {108}, {112}, {116}, {120}, {124}, {128},
                     {132}, {136}, {140}, {144}, {149}, {153}, {157}, {161}, {165}};
    auto complete = membership.plan(all, 36);
    CHECK(complete.uncovered_count == 0 && complete.assignments[0].count == 36);
    Command wide;
    wide.count = 36;
    wide.channels = complete.assignments[0].channels;
    parent.prepare(1, wide);
    CHECK(rebooted.apply(wide, true));
    for (auto channel : all)
        rebooted.tuned(channel.number);
    CHECK(rebooted.status().confirmed_channels == ((uint64_t(1) << 36) - 1));
    CHECK(parent.observe(1, 11, rebooted.report(), 79));
    CHECK(parent.coverage(complete, 80, 100).confirmed == 36);
    Packet wide_packet;
    Command wide_decoded;
    CHECK(pack_command(wide, wide_packet) && unpack_command(wide_packet, wide_decoded));
    CHECK(same_assignment(wide, wide_decoded));
    for (bool observation : {false, true}) {
        Report wide_report, wide_result;
        wide_report.scan = rebooted.report();
        wide_report.has_observation = observation;
        wide_report.observation.rssi = -50;
        CHECK(pack_report(wide_report, wide_packet));
        CHECK(unpack_report(wide_packet, wide_result));
        CHECK(wide_result.scan.confirmed_channels == wide_report.scan.confirmed_channels);
    }
    // Bit 31 is valid; never shift by 32 when validating a full assignment.
    command.count = 32;
    parent.prepare(1, command);
    ScanStatus full{command.assignment_id, UINT32_MAX, ScanState::scanning, 0};
    CHECK(parent.observe(1, 11, full, 80));
    command.count = 1;
    parent.prepare(1, command);
    full.assignment_id = command.assignment_id;
    CHECK(!parent.observe(1, 11, full, 81));
    command.count = 0;
    CHECK(rebooted.apply(command, false));
    CHECK(rebooted.status().state == ScanState::idle);
    command.follow = true;
    command.target_radio = Radio::ble;
    CHECK(rebooted.apply(command, false));
    CHECK(rebooted.status().state == ScanState::idle);
    // Wire roundtrips include empty reports, status, ACK and generation.
    Packet packet;
    Command decoded;
    CHECK(pack_command(command, packet));
    CHECK(unpack_command(packet, decoded));
    CHECK(decoded.assignment_id == command.assignment_id);
    Report report, result;
    report.scan = {7, 3, ScanState::scanning, 0};
    CHECK(pack_report(report, packet));
    CHECK(unpack_report(packet, result));
    CHECK(!result.has_observation && result.scan.assignment_id == 7 &&
          result.scan.confirmed_channels == 3);
    for (uint16_t i = 0; i < packet.size; ++i) {
        Packet truncated = packet;
        truncated.size = i;
        CHECK(!unpack_report(truncated, result));
    }
    packet.payload[16] = 99;
    CHECK(!unpack_report(packet, result));
    report.scan = {7, 0, ScanState::error, 36};
    report.has_observation = true;
    report.observation.rssi = -50;
    CHECK(pack_report(report, packet));
    CHECK(unpack_report(packet, result));
    CHECK(result.scan.failed_channel == 36 && result.has_observation);
    report.scan.confirmed_channels = 1;
    CHECK(!pack_report(report, packet));
    std::puts("Scan status tests passed");
}
