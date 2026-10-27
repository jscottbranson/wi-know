#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "platform.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
using namespace wiknow;
namespace c = wiknow::config;
static Membership members;
static Tracker tracker(c::trend_config);
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
        char event[384];
        std::snprintf(event, sizeof(event),
                      "{\"event\":\"following_alert\",\"radio\":\"%s\","
                      "\"address\":\"%s\",\"address_type\":%u,\"at_ms\":%llu,"
                      "\"displacement_m\":%.1f,\"delay_ms\":%d}\n",
                      o.radio == Radio::wifi ? "wifi" : "ble", following_address,
                      unsigned(o.address_type), static_cast<unsigned long long>(o.at_ms),
                      distance_m, c::following_alert_delay_ms);
        platform::write_log(event);
        if (!has_beeped || received - last_beep >= uint64_t(c::buzzer_cooldown_ms)) {
            platform::indicators(target_trend, true);
            last_beep = received;
            has_beeped = true;
        }
    }
    Match match = best_match(c::rules.data(), c::rules.size(), o);
    // Preserve matching target state even when logging is matches-only.
    if (!match.rule && std::strcmp(c::logging_mode, "all") != 0)
        return;
    Track &track = tracker.observe(o, fix);
    if (match.rule && platform::now_ms() - o.at_ms <= uint64_t(c::signal_trend_stale_ms)) {
        bool current = target_rule && same_target(target, o);
        bool expired = !target_rule ||
                       platform::now_ms() - target_seen >= uint64_t(c::follower_release_after_ms);
        if (current || expired || match.rule->priority >= target_rule->priority) {
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
                    tracker.reset_trend(track);
                target = o;
                target_rule = match.rule;
                target_seen = o.at_ms;
                target_peak = track.peak.rssi;
                target_trend = tracker.trend(track, platform::now_ms());
            } else
                target_seen = o.at_ms;
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
    if (observation_json(o, track, match, fix, gps, json, sizeof(json)))
        platform::write_log(json);
    else
        ++platform::dropped;
}
static void parent_loop() {
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
        members.expire(now, c::bus_membership_lease_ms);
        if (target_rule && now - target_seen >= uint64_t(c::follower_release_after_ms)) {
            target_rule = nullptr;
            target_trend = Trend::unknown;
        }
        if (c::bus_enabled) {
            bool more = false;
            if (!burst)
                burst_started = platform::now_ms();
            auto plan = members.plan(c::channels.data(), c::channels.size());
            Command command{};
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
            Packet request{};
            request.node = next_node;
            request.boot = platform::boot_id;
            request.sequence = ++sequence;
            pack_command(command, request);
            uint64_t sent = platform::now_ms();
            platform::bus_send(request);
            Packet response{};
            if (platform::bus_receive(response, c::bus_poll_timeout_ms) &&
                response.node == next_node && response.sequence == sequence) {
                Report report{};
                if (unpack_report(response, report)) {
                    uint64_t received = platform::now_ms();
                    members.seen(Node{next_node, response.boot, report.role, report.band24,
                                      report.band5, true, received},
                                 received);
                    if (report.has_observation) {
                        more = true;
                        auto &o = report.observation;
                        uint64_t delay = received - sent;
                        o.at_ms = report.capture_age_ms <= received
                                      ? received - report.capture_age_ms
                                      : 0;
                        o.timing_uncertainty_ms = uint32_t(std::min<uint64_t>(UINT32_MAX, delay));
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
        Observation o{};
        for (int i = 0; i < 16 && xQueueReceive(platform::observations, &o, 0) == pdTRUE; ++i)
            consume(o);
        now = platform::now_ms();
        Trend visible = target_rule && now >= target.at_ms &&
                                now - target.at_ms <= uint64_t(c::signal_trend_stale_ms)
                            ? target_trend
                            : Trend::unknown;
        platform::indicators(visible, false);
        if (now - last_display >= 250) {
            char line1[32]{}, line2[32]{};
            size_t page = (now / c::display_page_interval_ms) % 3;
            if (following_visible && now - following_alert_at < 30000) {
                std::snprintf(line1, sizeof(line1), "Following alert");
                std::snprintf(line2, sizeof(line2), "%.16s", following_address);
            } else if (page == 0) {
                auto plan = members.plan(c::channels.data(), c::channels.size());
                std::snprintf(line1, sizeof(line1), "Children:%u", unsigned(plan.count));
                std::snprintf(line2, sizeof(line2), "Gap:%u SD:%s", unsigned(plan.uncovered_count),
                              platform::storage_ready() ? "OK" : "OFF");
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
            } else {
                bool fresh = gps.nearest(now, c::gps_max_fix_age_ms).valid;
                std::snprintf(line1, sizeof(line1), "GPS:%s", fresh ? "FIX" : "NO FIX");
                std::snprintf(line2, sizeof(line2), "UTC:%s D:%u", gps.has_time() ? "GPS" : "?",
                              unsigned(platform::dropped.load()));
            }
            platform::screen(line1, line2);
            last_display = now;
        }
        if (now - last_status >= 5000) {
            auto plan = members.plan(c::channels.data(), c::channels.size());
            ESP_LOGI("parent", "children=%u uncovered=%u drops=%u SD=%s", unsigned(plan.count),
                     unsigned(plan.uncovered_count), unsigned(platform::dropped.load()),
                     platform::storage_ready() ? "ready" : "off");
            last_status = now;
        }
        vTaskDelay(platform::ticks(5));
    }
}
static void worker_loop() {
    Command assignment{};
    uint64_t expires = 0, last_tune = 0, last_target = 0;
    size_t cursor = 0;
    uint8_t tuned = 0;
    uint32_t parent_boot = 0, last_command_sequence = 0;
    bool have_parent = false;
    for (;;) {
        Packet request{};
        if (c::bus_enabled && platform::bus_receive(request, 10) && request.node == c::node_id) {
            Command command{};
            if (unpack_command(request, command) &&
                (!have_parent || request.boot != parent_boot ||
                 sequence_newer(request.sequence, last_command_sequence))) {
                have_parent = true;
                parent_boot = request.boot;
                last_command_sequence = request.sequence;
                bool changed = command.count != assignment.count ||
                               command.follow != assignment.follow ||
                               command.target != assignment.target ||
                               command.target_radio != assignment.target_radio ||
                               command.target_channel != assignment.target_channel ||
                               command.target_address_type != assignment.target_address_type;
                for (size_t i = 0; i < command.count && !changed; ++i)
                    changed = !(command.channels[i] == assignment.channels[i]);
                assignment = command;
                expires = platform::now_ms() + command.lease_ms;
                if (changed) {
                    cursor = 0;
                    last_tune = 0;
                    last_target = platform::now_ms();
                    xQueueReset(platform::observations);
                }
                Report report{};
                report.role = c::role;
                report.dropped = platform::dropped;
                Observation o{};
                uint64_t now = platform::now_ms();
                for (int i = 0; i < 32 && xQueueReceive(platform::observations, &o, 0) == pdTRUE;
                     ++i) {
                    bool wanted = c::role == Role::child && assignment.count;
                    if (c::role == Role::follower && assignment.follow)
                        wanted = o.radio == assignment.target_radio &&
                                 o.address == assignment.target &&
                                 o.address_type == assignment.target_address_type;
                    if (wanted && o.at_ms <= now) {
                        report.has_observation = true;
                        report.observation = o;
                        report.capture_age_ms =
                            uint32_t(std::min<uint64_t>(UINT32_MAX, now - o.at_ms));
                        if (c::role == Role::follower)
                            last_target = o.at_ms;
                        break;
                    }
                }
                Packet response{};
                response.node = c::node_id;
                response.boot = platform::boot_id;
                response.sequence = request.sequence;
                pack_report(report, response);
                platform::bus_send(response);
            }
        }
        uint64_t now = platform::now_ms();
        if (now >= expires) {
            assignment.count = 0;
            assignment.follow = false;
            platform::pause_wifi();
            tuned = 0;
            Observation ignored{};
            for (int i = 0; i < 32 && xQueueReceive(platform::observations, &ignored, 0) == pdTRUE;
                 ++i) {
            }
        }
        if (now - last_tune >= assignment.dwell_ms || !last_tune) {
            uint8_t channel = 0;
            if (c::role == Role::child && assignment.count)
                channel = assignment.channels[cursor++ % assignment.count].number;
            if (c::role == Role::follower && assignment.follow &&
                assignment.target_radio == Radio::wifi) {
                channel = now - last_target < 3000
                              ? assignment.target_channel
                              : c::channels[cursor++ % c::channels.size()].number;
            }
            if (!channel) {
                platform::pause_wifi();
                tuned = 0;
            } else if (channel != tuned) {
                if (platform::tune(channel))
                    tuned = channel;
                else {
                    platform::pause_wifi();
                    tuned = 0;
                    assignment.count = 0;
                    assignment.follow = false;
                }
            }
            last_tune = now;
        }
        vTaskDelay(platform::ticks(5));
    }
}
} // namespace
extern "C" void app_main() {
    // An NVS error is surfaced instead of silently erasing stored state.
    ESP_ERROR_CHECK(nvs_flash_init());
    platform::boot_id = esp_random();
    platform::observations = xQueueCreate(32, sizeof(wiknow::Observation));
    configASSERT(platform::observations);
    ESP_LOGI("wi-know", "role=%s node=%d enabled rules=%u", wiknow::config::node_role,
             wiknow::config::node_id,
             unsigned(std::count_if(wiknow::config::rules.begin(), wiknow::config::rules.end(),
                                    [](const wiknow::Rule &r) { return r.enabled; })));
    platform::bus_start();
    platform::start_peripherals();
    platform::start_radios();
    if (wiknow::config::role == wiknow::Role::parent)
        parent_loop();
    else
        worker_loop();
}
