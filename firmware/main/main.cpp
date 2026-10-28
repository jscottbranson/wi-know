#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "log_helpers.hpp"
#include "nvs_flash.h"
#include "platform.hpp"
#include "wiknow/remote_id.hpp"
#include "wiknow/report_delivery.hpp"
#include "wiknow/scan_status.hpp"
#include "wiknow/worker_counters.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace {
using namespace wiknow;
namespace c = wiknow::config;
static Membership members;
static std::array<ReportReceipt, max_nodes> report_receipts;
static std::array<DeviceId, max_nodes> receipt_devices;
static DiscoveryTable discovery(c::bus_first_node_id, c::bus_last_node_id);
static WorkerIdentity worker_identity;
static Identity last_probe;
static bool parent_conflict{};
static uint32_t discovery_failures{}, discovery_token{};
static Identity parent_identity() {
    Identity id{};
    id.parent = platform::device_id;
    id.parent_boot = platform::boot_id;
    return id;
}
static bool conflict(const Packet &packet) {
    if (controller_conflict(packet, platform::device_id, platform::boot_id)) {
        parent_conflict = true;
        ESP_LOGE("discovery", "Another parent detected; bus control stopped until reboot");
    }
    return parent_conflict;
}
static bool receive_parent(Packet &packet, uint32_t timeout) {
    const auto until = platform::now_ms() + timeout;
    do {
        if (platform::bus_receive(packet, 1)) {
            if (c::universal_enabled && conflict(packet))
                return false;
            return true;
        }
    } while (platform::now_ms() < until);
    return false;
}
static void discovery_step() {
    static uint8_t slot{};
    static uint32_t nonce{};
    static uint64_t next{};
    const auto now = platform::now_ms();
    if (now < next || parent_conflict)
        return;
    // One bounded response opportunity, interleaved with normal worker service.
    next = now + 100 + (esp_random() % 51);
    // An actual randomized receive interval (not an overdue deadline) breaks
    // lockstep even at slow baud rates or while assignment retries are busy.
    const auto quiet_until = platform::now_ms() + 1 + (esp_random() % 64);
    Packet unsolicited{};
    do {
        if (platform::bus_receive(unsolicited, 1) && conflict(unsolicited))
            return;
    } while (platform::now_ms() < quiet_until);
    discovery.expire(platform::now_ms());
    discovery.roles(c::follower_enabled, discovery_token);
    if (!slot)
        nonce = esp_random();
    Identity query = parent_identity();
    query.slot = slot;
    query.nonce = nonce;
    Packet packet{};
    pack_identity(query, packet, MessageType::discover);
    platform::bus_send(packet);
    Packet response{};
    Identity hello{};
    if (receive_parent(response, c::bus_poll_timeout_ms) && response.type == MessageType::hello &&
        unpack_identity(response, hello) && hello.parent == query.parent &&
        hello.parent_boot == query.parent_boot && hello.nonce == nonce && hello.slot == slot &&
        discovery_slot(hello.worker, hello.worker_boot, nonce) == slot) {
        auto *offered = discovery.offer(hello, platform::now_ms(), c::bus_assignment_lease_ms,
                                        ++discovery_token);
        if (offered && receipt_devices[offered->node - 1] != hello.worker) {
            report_receipts[offered->node - 1] = {};
            receipt_devices[offered->node - 1] = hello.worker;
        }
        if (!offered) {
            ++discovery_failures;
            ESP_LOGW("discovery", "Capacity exhausted or duplicate identity; request deferred");
        }
        discovery.roles(c::follower_enabled, discovery_token);
    }
    slot = (slot + 1) % discovery_slots;
    if (parent_conflict)
        return;
    for (const auto &entry : discovery.nodes())
        if (entry.node && !entry.acknowledged) {
            auto *node = discovery.get(entry.node);
            node->identity.nonce = nonce;
            node->identity.slot = query.slot;
            Packet offer{};
            offer.node = node->node;
            pack_identity(node->identity, offer, MessageType::assign);
            platform::bus_send(offer);
            Identity ack{};
            if (receive_parent(response, c::bus_poll_timeout_ms) &&
                response.type == MessageType::assigned && response.node == node->node &&
                unpack_identity(response, ack) &&
                discovery.acknowledge(node->node, ack, platform::now_ms(),
                                      c::bus_assignment_lease_ms)) {
                ESP_LOGI("discovery",
                         "Assigned %02x:%02x:%02x:%02x:%02x:%02x node=%u role=%s token=%u",
                         ack.worker[0], ack.worker[1], ack.worker[2], ack.worker[3], ack.worker[4],
                         ack.worker[5], unsigned(node->node),
                         ack.role == Role::follower ? "follower" : "child", unsigned(ack.token));
            } else
                ++discovery_failures;
            break;
        }
    // Remove stale role/capability evidence immediately, keeping only current sessions.
    members = current_membership(members, discovery);
}

static WorkerLossCounters worker_losses;
static ScanCoverage scan_coverage;
// Parent-only state, accessed from the normal parent task (never an ISR or a
// cache-disabled callback). Keep this large table out of scarce internal RAM
// when PSRAM is available; workers must not reserve it at all.
static Tracker *tracker{};
static void init_tracker() {
    if (platform::role != Role::parent)
        return;
    void *memory = heap_caps_malloc(sizeof(Tracker), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const bool external = memory != nullptr;
    if (!memory)
        memory = heap_caps_malloc(sizeof(Tracker), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) {
        ESP_LOGE("memory", "Cannot allocate parent tracking table (%u bytes)",
                 unsigned(sizeof(Tracker)));
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    tracker = new (memory) Tracker(c::trend_config);
    ESP_LOGI("memory", "Parent tracker: %u bytes in %s; PSRAM free=%u", unsigned(sizeof(Tracker)),
             external ? "PSRAM" : "internal RAM",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}
static Gps gps;
static FollowingDetector following(c::following_config, c::following_ignore.data(),
                                   c::following_ignore.size());
static bool following_visible{};
static uint64_t following_alert_at{};
static char following_address[18]{};
static Observation target{};
static const Rule *target_rule{};
static uint64_t target_seen{}, last_beep{};
static bool has_beeped{};
static int target_peak{};
static Trend target_trend{Trend::unknown};
static bool same_target(const Observation &a, const Observation &b) {
    return a.radio == b.radio && a.address_type == b.address_type && a.address == b.address;
}
static void log_worker_losses(uint16_t node, uint64_t now) {
    const auto *worker = worker_losses.get(node);
    if (!worker)
        return;
    char event[384];
    std::snprintf(event, sizeof(event),
                  "{\"event\":\"worker_status\",\"node\":%u,\"boot\":%u,\"online\":%s,"
                  "\"reported_drops\":%u,\"worker_lifetime_drops\":%llu,"
                  "\"worker_drops_total\":%llu,\"at_ms\":%llu}\n",
                  unsigned(node), unsigned(worker->boot), worker->online ? "true" : "false",
                  unsigned(worker->reported), static_cast<unsigned long long>(worker->total),
                  static_cast<unsigned long long>(worker_losses.total()),
                  static_cast<unsigned long long>(now));
    platform::write_log(event);
}
static void log_worker_scan(uint16_t node, uint64_t now) {
    const auto *worker = scan_coverage.get(node);
    if (!worker || !worker->received)
        return;
    char event[384];
    std::snprintf(event, sizeof(event),
                  "{\"event\":\"worker_scan\",\"node\":%u,\"boot\":%u,"
                  "\"assignment_id\":%u,\"scan_state\":\"%s\",\"confirmed_channel_bits\":%u,"
                  "\"failed_channel\":%u,\"at_ms\":%llu}\n",
                  unsigned(node), unsigned(worker->boot), unsigned(worker->scan.assignment_id),
                  scan_state_name(worker->scan.state), unsigned(worker->scan.confirmed_channels),
                  unsigned(worker->scan.failed_channel), static_cast<unsigned long long>(now));
    platform::write_log(event);
}
static void consume(Observation o) {
    o.receiver_boot = platform::boot_id;
    // Drain GPS again after a bus poll so buffered worker captures use the latest fixes.
    platform::gps_update(gps);
    Fix fix = gps.tag(o, c::gps_max_fix_age_ms, c::gps_max_timing_uncertainty_ms);
    uint64_t received = platform::now_ms();
    double distance_m = 0;
    Fix following_fix = fix;
    // Buffered captures must not trigger an alert after the device has disappeared.
    if (o.at_ms > received || received - o.at_ms > uint64_t(c::following_alert_max_gap_ms))
        following_fix.valid = false;
    if (following.observe(o, following_fix, distance_m)) {
        std::snprintf(following_address, sizeof(following_address), "%02X:%02X:%02X:%02X:%02X:%02X",
                      o.address[0], o.address[1], o.address[2], o.address[3], o.address[4],
                      o.address[5]);
        following_visible = true;
        following_alert_at = received;
        char event[1024];
        if (platform::following_alert_json(o, fix, gps, distance_m, c::following_alert_delay_ms,
                                           event, sizeof(event)))
            platform::write_log(event);
        else
            ++platform::dropped;
        if (!has_beeped || received - last_beep >= uint64_t(c::buzzer_cooldown_ms)) {
            platform::indicators(target_trend, true);
            last_beep = received;
            has_beeped = true;
        }
    }
    Match match = best_match(c::rules.data(), c::rules.size(), o);
    // Preserve matching target state even when logging is matches-only.
    const auto drone = remote_id_messages(o);
    if (!match.rule && !drone.count && std::strcmp(c::logging_mode, "all") != 0)
        return;
    Track &track = tracker->observe(o, fix);
    if (match.rule && received >= o.at_ms &&
        received - o.at_ms <= uint64_t(c::signal_trend_stale_ms)) {
        bool current = target_rule && same_target(target, o);
        bool expired = !target_rule ||
                       platform::now_ms() - target_seen >= uint64_t(c::follower_release_after_ms);
        if ((current && o.at_ms >= target.at_ms) || expired ||
            (!current &&
             (match.rule->priority > target_rule->priority ||
              (match.rule->priority == target_rule->priority && o.at_ms >= target_seen)))) {
            // Prefer the dedicated follower's measurements to avoid source-switch flicker.
            bool existing_follower = false, incoming_follower = false;
            for (const auto &n : members.nodes()) {
                if (n.id == target.node && n.role == Role::follower)
                    existing_follower = true;
                if (n.id == o.node && n.role == Role::follower)
                    incoming_follower = true;
            }
            if (!current || !existing_follower || incoming_follower ||
                platform::now_ms() - target.at_ms > uint64_t(c::signal_trend_stale_ms)) {
                if (!target_rule || !same_signal(target, o))
                    tracker->reset_trend(track);
                target = o;
                target_rule = match.rule;
                target_seen = current ? std::max(target_seen, o.at_ms) : o.at_ms;
                target_peak = track.peak.rssi;
                target_trend = tracker->trend(track, platform::now_ms());
            } else
                target_seen = std::max(target_seen, o.at_ms);
        }
        uint64_t now = platform::now_ms();
        if (match.rule->buzzer &&
            (!has_beeped || now - last_beep >= uint64_t(c::buzzer_cooldown_ms))) {
            platform::indicators(target_trend, true);
            last_beep = now;
            has_beeped = true;
        }
    }
    char json[log_line_capacity];
    for (size_t i = 0; i < drone.count; ++i) {
        if (drone_detection_json(o, i, fix, gps, json, sizeof(json)))
            platform::write_log(json);
        else
            ++platform::dropped;
    }
    // Preserve every matching classification; actions still use the highest priority.
    // Repeated records share one observation identity and do not increment its count.
    if (match.rule) {
        for (const auto &rule : c::rules)
            if (matches(rule, o)) {
                if (observation_json(o, track, Match{&rule}, fix, gps, json, sizeof(json)))
                    platform::write_log(json);
                else
                    ++platform::dropped;
            }
    } else if (observation_json(o, track, {}, fix, gps, json, sizeof(json)))
        platform::write_log(json);
    else
        ++platform::dropped;
}
static void parent_loop() {
    if (c::universal_enabled) {
        discovery_token = esp_random() | 1u;
        // Listen before any transmission; stagger simultaneous power-up.
        const auto until = platform::now_ms() + 2000 + (esp_random() % 2000);
        Packet incoming{};
        while (platform::now_ms() < until && !parent_conflict)
            if (platform::bus_receive(incoming, 10))
                conflict(incoming);
    }
    uint16_t next_node = c::bus_first_node_id;
    uint32_t sequence = 0;
    unsigned burst = 0;
    uint64_t burst_started = 0;
    constexpr uint32_t node_count = c::bus_last_node_id - c::bus_first_node_id + 1;
    // Spend only lease slack on extra polls, preserving the validated sweep bound.
    constexpr uint32_t node_budget_ms = c::bus_membership_lease_ms / (2 * node_count);
    uint64_t last_display = 0, last_status = 0;
    for (;;) {
        platform::gps_update(gps);
        uint64_t now = platform::now_ms();
        if (members.expire(now, c::bus_membership_lease_ms))
            for (const auto &node : members.nodes())
                if (!node.online && worker_losses.offline(node.id)) {
                    scan_coverage.offline(node.id);
                    log_worker_losses(node.id, now);
                }
        if (target_rule && now - target_seen >= uint64_t(c::follower_release_after_ms)) {
            target_rule = nullptr;
            target_trend = Trend::unknown;
        }
        if (c::universal_enabled) {
            discovery_step();
            for (uint16_t id = c::bus_first_node_id; id <= c::bus_last_node_id; ++id) {
                bool online = false;
                for (const auto &n : members.nodes())
                    if (n.id == id && n.online)
                        online = true;
                if (!online) {
                    scan_coverage.offline(id);
                    if (worker_losses.offline(id))
                        log_worker_losses(id, now);
                }
            }
            bool scanners = false;
            for (const auto &n : members.nodes())
                if (n.online && n.role == Role::child)
                    scanners = true;
            platform::local_scanning(!scanners);
        }
        auto *session = c::universal_enabled ? discovery.get(next_node) : nullptr;
        if (c::bus_enabled && !parent_conflict &&
            (!c::universal_enabled || (session && session->node && session->acknowledged))) {
            bool more = false;
            if (!burst)
                burst_started = platform::now_ms();
            auto plan = members.plan(c::channels.data(), c::channels.size());
            Command command{};
            command.ack = report_receipts[next_node - 1].acknowledgement();
            command.dwell_ms = c::wifi_dwell_ms;
            command.lease_ms = c::bus_assignment_lease_ms;
            for (size_t i = 0; i < plan.count; ++i)
                if (plan.assignments[i].node == next_node) {
                    command.channels = plan.assignments[i].channels;
                    command.count = uint8_t(plan.assignments[i].count);
                }
            uint16_t follower = 0;
            for (const auto &n : members.nodes())
                if (n.online && n.role == Role::follower && (!follower || n.id < follower))
                    follower = n.id;
            if (c::follower_enabled && target_rule && target_rule->follow &&
                next_node == follower) {
                command.follow = true;
                command.target = target.address;
                command.target_radio = target.radio;
                command.target_channel = target.channel;
                command.target_address_type = target.address_type;
            }
            scan_coverage.prepare(next_node, command);
            Packet request{};
            request.node = next_node;
            request.boot = platform::boot_id;
            request.sequence = ++sequence;
            pack_command(command, request);
            if (c::universal_enabled)
                pack_identity(session->identity, request, MessageType::session_poll);
            uint64_t sent = platform::now_ms();
            platform::bus_send(request);
            Packet response{};
            if (receive_parent(response, c::bus_poll_timeout_ms) && response.node == next_node &&
                response.sequence == sequence) {
                Report report{};
                bool valid = true;
                if (c::universal_enabled) {
                    Identity identity{};
                    Packet inner{};
                    valid = response.type == MessageType::session_report &&
                            unpack_identity(response, identity, &inner) &&
                            same_session(session->identity, identity) &&
                            response.boot == identity.worker_boot;
                    if (valid)
                        response = inner;
                }
                if (valid && unpack_report(response, report) &&
                    (!c::universal_enabled || report.role == session->identity.role)) {
                    if (c::universal_enabled)
                        discovery.acknowledge(next_node, session->identity, platform::now_ms(),
                                              c::bus_assignment_lease_ms);
                    uint64_t received = platform::now_ms();
                    members.seen(Node{next_node, response.boot, report.role, report.band24,
                                      report.band5, true, received},
                                 received);
                    if (worker_losses.update(next_node, response.boot, report.dropped))
                        log_worker_losses(next_node, received);
                    if (scan_coverage.observe(next_node, response.boot, report.scan, received))
                        log_worker_scan(next_node, received);
                    if (report.has_observation) {
                        more = true;
                        auto &o = report.observation;
                        uint64_t delay = received - sent;
                        o.at_ms = report.capture_age_ms <= received
                                      ? received - report.capture_age_ms
                                      : 0;
                        o.timing_uncertainty_ms = uint32_t(std::min<uint64_t>(UINT32_MAX, delay));
                        if (report_receipts[next_node - 1].accept(o))
                            consume(o);
                    }
                }
            }
            // Drain busy workers in bounded bursts; still service local input each iteration.
            if (!more || ++burst >= 8 ||
                platform::now_ms() - burst_started + c::bus_poll_timeout_ms + 5 >= node_budget_ms) {
                burst = 0;
                if (++next_node > c::bus_last_node_id)
                    next_node = c::bus_first_node_id;
            }
        }
        if (c::universal_enabled && (!session || !session->node || !session->acknowledged)) {
            burst = 0;
            if (++next_node > c::bus_last_node_id)
                next_node = c::bus_first_node_id;
        }
        Observation o{};
        for (int i = 0; i < 16 && platform::take_observation(o); ++i)
            consume(o);
        now = platform::now_ms();
        Trend visible = target_rule && now >= target.at_ms &&
                                now - target.at_ms <= uint64_t(c::signal_trend_stale_ms)
                            ? target_trend
                            : Trend::unknown;
        platform::indicators(visible, false);
        if (now - last_display >= 250) {
            char line1[32]{}, line2[32]{};
            size_t page = (now / c::display_page_interval_ms) % 5;
            if (following_visible && now - following_alert_at < 30000) {
                std::snprintf(line1, sizeof(line1), "Following alert");
                std::snprintf(line2, sizeof(line2), "%.16s", following_address);
            } else if (page == 0) {
                auto plan = members.plan(c::channels.data(), c::channels.size());
                std::snprintf(line1, sizeof(line1), "Children:%u", unsigned(plan.count));
                auto coverage = scan_coverage.coverage(plan, now, c::bus_membership_lease_ms);
                std::snprintf(line2, sizeof(line2), "Tuned:%u/%u", unsigned(coverage.confirmed),
                              unsigned(c::channels.size()));
            } else if (page == 1) {
                std::snprintf(line1, sizeof(line1), "%.16s",
                              target_rule ? target_rule->id.data() : "No matching rule");
                if (target_rule && now - target.at_ms > uint64_t(c::signal_trend_stale_ms))
                    std::snprintf(line2, sizeof(line2), "Signal lost");
                else if (target_rule)
                    std::snprintf(line2, sizeof(line2), "%d P%d %.6s", int(target.rssi),
                                  target_peak, trend_name(visible));
                else
                    std::snprintf(line2, sizeof(line2), "Add rules/*.json");
            } else if (page == 2) {
                bool fresh = gps.nearest(now, c::gps_max_fix_age_ms).valid;
                std::snprintf(line1, sizeof(line1), "GPS:%s", fresh ? "FIX" : "NO FIX");
                std::snprintf(line2, sizeof(line2), "UTC:%s", gps.has_time() ? "GPS" : "?");
            } else if (page == 3) {
                std::snprintf(line1, sizeof(line1), "Local:%u", unsigned(platform::dropped.load()));
                if (worker_losses.total() > 999999999)
                    std::snprintf(line2, sizeof(line2), "Worker:>=1B");
                else
                    std::snprintf(line2, sizeof(line2), "Worker:%llu",
                                  static_cast<unsigned long long>(worker_losses.total()));
            }
            if (page == 4 && !(following_visible && now - following_alert_at < 30000)) {
                auto plan = members.plan(c::channels.data(), c::channels.size());
                auto coverage = scan_coverage.coverage(plan, now, c::bus_membership_lease_ms);
                std::snprintf(line1, sizeof(line1), "Plan:%u Err:%u", unsigned(coverage.planned),
                              unsigned(coverage.errors));
                std::snprintf(line2, sizeof(line2), "Gap:%u %s", unsigned(plan.uncovered_count),
                              platform::storage_name());
            }
            if ((c::standalone_enabled || (c::universal_enabled && platform::local_scanning())) &&
                page == 0 && !(following_visible && now - following_alert_at < 30000)) {
                const int channel = platform::standalone_scan_channel();
                std::snprintf(line1, sizeof(line1), "Standalone");
                if (channel > 0)
                    std::snprintf(line2, sizeof(line2), "Wi-Fi CH:%d", channel);
                else
                    std::snprintf(line2, sizeof(line2), "%s",
                                  channel == 0    ? "BLE scan"
                                  : channel == -2 ? "Starting scan"
                                                  : "Scan error");
            }
            if (c::universal_enabled && page == 3) {
                std::snprintf(line1, sizeof(line1), "Parent %02X%02X%02X", platform::device_id[3],
                              platform::device_id[4], platform::device_id[5]);
                std::snprintf(line2, sizeof(line2), "Join errors:%u", unsigned(discovery_failures));
            }
            if (parent_conflict) {
                std::snprintf(line1, sizeof(line1), "PARENT CONFLICT");
                std::snprintf(line2, sizeof(line2), "Bus stopped");
            }
            platform::screen(line1, line2);
            last_display = now;
        }
        if (now - last_status >= 5000) {
            auto plan = members.plan(c::channels.data(), c::channels.size());
            auto coverage = scan_coverage.coverage(plan, now, c::bus_membership_lease_ms);
            if (c::standalone_enabled || (c::universal_enabled && platform::local_scanning()))
                ESP_LOGI("parent",
                         "standalone scan_channel=%d (0=BLE -1=error -2=starting) local_drops=%u "
                         "storage=%s",
                         platform::standalone_scan_channel(), unsigned(platform::dropped.load()),
                         platform::storage_name());
            else
                ESP_LOGI(
                    "parent",
                    "children=%u planned=%u tuned=%u scan_errors=%u uncovered=%u local_drops=%u "
                    "worker_drops=%llu storage=%s",
                    unsigned(plan.count), unsigned(coverage.planned), unsigned(coverage.confirmed),
                    unsigned(coverage.errors), unsigned(plan.uncovered_count),
                    unsigned(platform::dropped.load()),
                    static_cast<unsigned long long>(worker_losses.total()),
                    platform::storage_name());
            last_status = now;
        }
        vTaskDelay(platform::ticks(5));
    }
}
static void worker_loop() {
    Command assignment{};
    RetainedObservation pending;
    WorkerScanProgress scan;
    uint64_t expires = 0, last_tune = 0, last_target = 0;
    size_t cursor = 0;
    uint8_t tuned = 0;
    uint32_t parent_boot = 0, last_command_sequence = 0;
    bool have_parent = false;
    bool worker_conflict = false;
    uint64_t last_identity_status = 0;
    auto update_radio = [&](uint64_t now) {
        if (now >= expires || (c::universal_enabled && !worker_identity.active(now))) {
            platform::pause_wifi();
            platform::set_capture_assignment(assignment, 0);
            tuned = 0;
            scan.expire();
            return;
        }
        if (scan.status().state == ScanState::error)
            return; // Retry on the next accepted poll; expose this failure first.
        if (now - last_tune >= assignment.dwell_ms || !last_tune) {
            uint8_t channel = 0;
            if (platform::role == Role::child && assignment.count)
                channel = assignment.channels[cursor++ % assignment.count].number;
            if (platform::role == Role::follower && assignment.follow &&
                assignment.target_radio == Radio::wifi) {
                channel = now - last_target < 3000
                              ? assignment.target_channel
                              : c::channels[cursor++ % c::channels.size()].number;
            }
            if (!channel) {
                platform::pause_wifi();
                tuned = 0;
            } else if (channel == tuned || platform::tune(channel)) {
                tuned = channel;
                scan.tuned(channel);
            } else {
                platform::pause_wifi();
                tuned = 0;
                scan.failed(channel);
                platform::set_capture_assignment(assignment, 0);
            }
            last_tune = now;
        }
    };
    for (;;) {
        Packet request{};
        bool received = c::bus_enabled && platform::bus_receive(request, 10);
        if (worker_conflict)
            received = false;
        if (c::universal_enabled && received) {
            if (request.type == MessageType::poll) {
                ESP_LOGE("discovery", "Incompatible manual controller; worker halted until reboot");
                worker_conflict = true;
                expires = 0;
            }
            Identity identity{};
            if (!unpack_identity(request, identity))
                received = false;
            else if (request.type == MessageType::discover) {
                if (worker_identity.active(platform::now_ms()) &&
                    identity.parent != worker_identity.identity.parent) {
                    ESP_LOGE("discovery", "Multiple parents; worker halted until reboot");
                    worker_conflict = true;
                    expires = 0;
                    received = false;
                } else {
                    last_probe = identity;
                    if (!worker_identity.active(platform::now_ms()) &&
                        discovery_slot(platform::device_id, platform::boot_id, identity.nonce) ==
                            identity.slot) {
                        identity.worker = platform::device_id;
                        identity.worker_boot = platform::boot_id;
                        identity.capabilities =
                            capability_wifi24 | capability_wifi5 | capability_follow;
                        Packet hello{};
                        pack_identity(identity, hello, MessageType::hello);
                        platform::bus_send(hello);
                    }
                }
                received = false;
            } else if (request.type == MessageType::assign) {
                const bool repeated = worker_identity.active(platform::now_ms()) &&
                                      request.node == worker_identity.node &&
                                      same_session(identity, worker_identity.identity);
                if (worker_identity.assign(identity, request.node, platform::device_id,
                                           platform::boot_id, last_probe, platform::now_ms(),
                                           c::bus_assignment_lease_ms)) {
                    if (!repeated) {
                        platform::set_runtime_role(identity.role, request.node);
                        expires = 0;
                        have_parent = false;
                        scan = {};
                    }
                    Packet ack{};
                    ack.node = request.node;
                    pack_identity(identity, ack, MessageType::assigned);
                    platform::bus_send(ack);
                    ESP_LOGI("discovery", "node=%u role=%s parent_session=%u token=%u",
                             unsigned(request.node),
                             identity.role == Role::follower ? "follower" : "child",
                             unsigned(identity.parent_boot), unsigned(identity.token));
                }
                received = false;
            } else if (request.type == MessageType::session_poll &&
                       worker_identity.accepts(identity, request.node, platform::now_ms()) &&
                       request.boot == identity.parent_boot) {
                Packet inner{};
                unpack_identity(request, identity, &inner);
                request = inner;
            } else
                received = false;
        }
        if (received && request.node == platform::node_id.load()) {
            Command command{};
            if (unpack_command(request, command) &&
                (!have_parent || request.boot != parent_boot ||
                 sequence_newer(request.sequence, last_command_sequence)) &&
                (!c::universal_enabled || worker_identity.accept_sequence(request.sequence))) {
                pending.acknowledge(command.ack);
                const bool parent_changed = !have_parent || request.boot != parent_boot;
                const bool changed = scan.apply(command, parent_changed);
                have_parent = true;
                parent_boot = request.boot;
                last_command_sequence = request.sequence;
                assignment = command;
                expires = platform::now_ms() + command.lease_ms;
                if (c::universal_enabled)
                    worker_identity.expires = platform::now_ms() + c::bus_assignment_lease_ms;
                platform::set_capture_assignment(assignment, expires);
                if (changed) {
                    cursor = 0;
                    last_tune = 0;
                    last_target = platform::now_ms();
                }
                update_radio(platform::now_ms());
                Report report{};
                report.scan = scan.report();
                report.role = platform::role.load();
                if (c::universal_enabled) {
                    report.band24 = worker_identity.identity.capabilities & capability_wifi24;
                    report.band5 = worker_identity.identity.capabilities & capability_wifi5;
                }
                report.dropped = platform::dropped;
                Observation o{};
                if (retain_next_observation(pending, o, platform::take_observation)) {
                    // Admission happened at capture time. Reassignment or lease
                    // expiry must not discard observations already accepted.
                    if (platform::role == Role::follower && assignment.follow &&
                        o.radio == assignment.target_radio && o.address == assignment.target &&
                        o.address_type == assignment.target_address_type && o.at_ms > last_target)
                        last_target = o.at_ms;
                }
                pending.report(report, platform::now_ms());
                Packet response{};
                response.node = platform::node_id.load();
                response.boot = platform::boot_id;
                response.sequence = request.sequence;
                pack_report(report, response);
                if (c::universal_enabled)
                    pack_identity(worker_identity.identity, response, MessageType::session_report);
                platform::bus_send(response);
            }
        }
        update_radio(platform::now_ms());
        if (c::universal_enabled && platform::now_ms() - last_identity_status >= 10000) {
            ESP_LOGI("discovery", "MAC=%02x:%02x:%02x:%02x:%02x:%02x state=%s node=%u role=%s",
                     platform::device_id[0], platform::device_id[1], platform::device_id[2],
                     platform::device_id[3], platform::device_id[4], platform::device_id[5],
                     worker_conflict                              ? "parent-conflict"
                     : worker_identity.active(platform::now_ms()) ? "assigned"
                                                                  : "awaiting-discovery",
                     unsigned(platform::node_id.load()),
                     platform::role == Role::follower ? "follower" : "child");
            last_identity_status = platform::now_ms();
        }
        vTaskDelay(platform::ticks(5));
    }
}
} // namespace
extern "C" void app_main() {
    // An NVS error is surfaced instead of silently erasing stored state.
    ESP_ERROR_CHECK(nvs_flash_init());
    platform::boot_id = esp_random();
    platform::detect_identity();
    init_tracker();
    platform::init_observation_queues();
    ESP_LOGI("wi-know", "role=%s node=%d enabled rules=%u", wiknow::config::node_role,
             unsigned(platform::node_id.load()),
             unsigned(std::count_if(wiknow::config::rules.begin(), wiknow::config::rules.end(),
                                    [](const wiknow::Rule &r) { return r.enabled; })));
    platform::bus_start();
    platform::start_peripherals();
    platform::start_radios();
    platform::start_storage();
    if (platform::role == wiknow::Role::parent)
        parent_loop();
    else
        worker_loop();
}
