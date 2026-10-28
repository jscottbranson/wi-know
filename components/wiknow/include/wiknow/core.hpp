#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wiknow {
constexpr size_t max_channels = 32, max_nodes = 16, max_rules = 64;
constexpr size_t max_tracks = 64, max_payload = 255;
constexpr size_t log_line_capacity = 2304;
// Serial numbers advance modulo 2^32; jumps of half the range are ambiguous.
constexpr bool sequence_newer(uint32_t candidate, uint32_t previous) {
    const uint32_t distance = candidate - previous;
    return distance != 0 && distance < 0x80000000u;
}
enum class Role : uint8_t { parent, child, follower };
enum class Radio : uint8_t { wifi, ble };
enum class Trend : uint8_t { unknown, increasing, decreasing, steady };
enum class Field : uint8_t { oui, ssid_prefix, name_prefix, manufacturer_prefix, service_uuid16 };
struct Channel {
    uint8_t number{};
    bool operator==(Channel b) const {
        return number == b.number;
    }
};
struct Fix {
    bool valid{};
    double latitude{}, longitude{};
    uint64_t at_ms{};
};
struct Observation {
    uint16_t node{};
    uint32_t boot{}, sequence{};
    uint32_t receiver_boot{};
    Radio radio{};
    uint8_t address_type{}, channel{}; // BLE 0=public, 1=random; Wi-Fi uses 0
    std::array<uint8_t, 6> address{};
    int8_t rssi{};
    uint64_t at_ms{}; // capture time mapped to parent's monotonic clock
    uint32_t timing_uncertainty_ms{};
    std::array<char, 33> ssid{}, name{};
    std::array<uint8_t, max_payload>
        payload{}; // BLE advertising data or Wi-Fi Remote ID vendor IE or NAN action body
    uint16_t payload_size{};
};
struct Predicate {
    Field field{};
    std::string_view value{};
};
struct Rule {
    std::string_view id{}, category{};
    Radio radio{};
    bool enabled{}, require_all{true};
    uint8_t priority{};
    bool buzzer{}, follow{};
    std::array<Predicate, 4> predicates{};
    size_t predicate_count{};
    std::string_view confidence{};
};
struct Match {
    const Rule *rule{};
};
bool parse_wifi(const uint8_t *frame, size_t length, Observation &out);
bool parse_ble(const uint8_t *data, size_t length, Observation &out);
bool matches(const Rule &rule, const Observation &observation);
Match best_match(const Rule *rules, size_t count, const Observation &observation);

struct Node {
    uint16_t id{};
    uint32_t boot{};
    Role role{Role::child};
    bool band24{}, band5{}, online{};
    uint64_t last_seen{};
};
struct Assignment {
    uint16_t node{};
    std::array<Channel, max_channels> channels{};
    size_t count{};
};
struct Plan {
    std::array<Assignment, max_nodes> assignments{};
    size_t count{};
    std::array<Channel, max_channels> uncovered{};
    size_t uncovered_count{};
};
class Membership {
  public:
    bool seen(Node node, uint64_t now);
    bool expire(uint64_t now, uint32_t lease_ms);
    Plan plan(const Channel *channels, size_t count) const;
    const std::array<Node, max_nodes> &nodes() const {
        return nodes_;
    }

  private:
    std::array<Node, max_nodes> nodes_{};
};

class Gps {
  public:
    bool feed(char byte, uint64_t now); // true when a valid RMC establishes UTC
    Fix nearest(uint64_t capture_ms, uint32_t max_age_ms) const;
    // Receiver location at capture time, allowing for worker-to-parent timing error.
    Fix tag(const Observation &observation, uint32_t max_age_ms,
            uint32_t max_timing_uncertainty_ms) const;
    bool utc(uint64_t at_ms, int64_t &utc_ms) const;
    bool has_time() const {
        return has_time_;
    }
    uint64_t last_sync() const {
        return time_at_;
    }

  private:
    bool sentence(uint64_t now);
    std::array<char, 128> line_{};
    size_t length_{};
    bool collecting_{};
    std::array<Fix, 32> fixes_{};
    size_t next_fix_{};
    bool has_time_{};
    uint64_t time_at_{};
    int64_t utc_at_{};
};

struct TrendConfig {
    uint32_t window_ms{2000}, stale_ms{6000};
    uint8_t minimum_samples{3}, confirmations{2};
    int deadband_db{3};
};
struct Track {
    bool used{};
    Observation latest{}, peak{};
    Fix peak_fix{};
    uint64_t first_seen{}, count{}, window_start{};
    std::array<uint32_t, 128> rssi_histogram{};
    size_t sample_count{};
    bool previous_valid{};
    int previous_median{};
    Trend trend{Trend::unknown}, candidate{Trend::unknown};
    uint8_t confirmations{};
    bool peak_changed{};
};
class Tracker {
  public:
    explicit Tracker(TrendConfig config = {}) : config_(config) {}
    Track &observe(const Observation &observation, Fix fix);
    void reset_trend(Track &track);
    Trend trend(const Track &track, uint64_t now) const;

  private:
    TrendConfig config_;
    std::array<Track, max_tracks> tracks_{};
};
bool same_signal(const Observation &a, const Observation &b);
const char *trend_name(Trend trend);

// Identity excludes receiver/channel so sightings from different nodes are combined.
struct DeviceIdentity {
    Radio radio{};
    uint8_t address_type{};
    std::array<uint8_t, 6> address{};
};
struct FollowingConfig {
    bool enabled{};
    uint32_t distance_m{200}, delay_ms{30000}, max_gap_ms{15000};
};
class FollowingDetector {
  public:
    FollowingDetector(FollowingConfig config, const DeviceIdentity *ignored, size_t count)
        : config_(config), ignored_(ignored), ignored_count_(count) {}
    // True once per continuous encounter, only on a fresh sighting beyond the radius.
    bool observe(const Observation &observation, Fix fix, double &distance_m);

  private:
    struct Entry {
        bool used{}, waiting{}, alerted{};
        DeviceIdentity identity{};
        Fix origin{};
        uint64_t last_seen{}, beyond_since{};
    };
    FollowingConfig config_;
    const DeviceIdentity *ignored_;
    size_t ignored_count_;
    std::array<Entry, max_tracks> entries_{};
};

// Versioned little-endian wire protocol. Not a packed native struct.
constexpr uint8_t protocol_version = 2;
enum class MessageType : uint8_t {
    poll = 1,
    report = 2,
    discover = 3,
    hello = 4,
    assign = 5,
    assigned = 6,
    session_poll = 7,
    session_report = 8
};
struct Packet {
    MessageType type{MessageType::poll};
    uint16_t node{};
    uint32_t boot{}, sequence{};
    uint16_t size{};
    std::array<uint8_t, 512> payload{};
};
constexpr size_t max_frame = 532;
size_t encode_packet(const Packet &packet, uint8_t *out, size_t capacity);
class Decoder {
  public:
    bool feed(uint8_t byte, Packet &out);
    void reset() {
        size_ = 0;
    }
    uint32_t errors() const {
        return errors_;
    }

  private:
    std::array<uint8_t, max_frame> data_{};
    size_t size_{};
    uint32_t errors_{};
};
struct Report;
struct ObservationAck {
    bool valid{};
    uint32_t boot{}, sequence{};
};
// One in-flight observation survives lost/corrupt replies and assignment changes.
class RetainedObservation {
  public:
    bool pending() const {
        return pending_;
    }
    bool retain(const Observation &observation);
    bool acknowledge(ObservationAck ack);
    void report(Report &out, uint64_t now) const;

  private:
    bool pending_{};
    Observation observation_{};
};
// Stop-and-wait delivery only repeats the most recently accepted observation.
class ReportReceipt {
  public:
    bool accept(const Observation &observation);
    ObservationAck acknowledgement() const {
        return ack_;
    }

  private:
    ObservationAck ack_{};
};
// Poll payload: role-independent assignment lease, dwell, channels, optional target.
struct Command {
    ObservationAck ack{};
    uint32_t assignment_id{};
    uint32_t lease_ms{10000}, dwell_ms{500};
    std::array<Channel, max_channels> channels{};
    uint8_t count{};
    bool follow{};
    Radio target_radio{};
    uint8_t target_channel{}, target_address_type{};
    std::array<uint8_t, 6> target{};
};
enum class ScanState : uint8_t { awaiting, idle, scanning, error, expired };
struct ScanStatus {
    uint32_t assignment_id{}, confirmed_channels{}; // bits index Command.channels
    ScanState state{ScanState::awaiting};
    uint8_t failed_channel{};
};
struct Report {
    ScanStatus scan{};
    Role role{Role::child};
    bool band24{true}, band5{true}, has_observation{};
    uint32_t dropped{}, capture_age_ms{};
    Observation observation{};
};
bool pack_command(const Command &command, Packet &packet);
bool unpack_command(const Packet &packet, Command &command);
bool pack_report(const Report &report, Packet &packet);
bool unpack_report(const Packet &packet, Report &report);

// JSONL is written outside radio callbacks. Returns false on insufficient capacity.
bool observation_json(const Observation &observation, const Track &track, Match match, Fix fix,
                      const Gps &gps, char *out, size_t capacity);
} // namespace wiknow
