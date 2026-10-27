#include "wiknow/core.hpp"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wiknow {
bool FollowingDetector::observe(const Observation &o, Fix fix, double &distance_m) {
    if (!config_.enabled)
        return false;
    auto equal = [&](const DeviceIdentity &id) {
        return id.radio == o.radio && id.address_type == o.address_type && id.address == o.address;
    };
    for (size_t i = 0; i < ignored_count_; ++i)
        if (equal(ignored_[i]))
            return false;
    Entry *entry = nullptr, *slot = &entries_[0];
    for (auto &e : entries_) {
        if (e.used && equal(e.identity)) {
            entry = &e;
            break;
        }
        if (!e.used || (slot->used && e.last_seen < slot->last_seen))
            slot = &e;
    }
    if (entry && o.at_ms <= entry->last_seen)
        return false; // duplicate or delayed worker report
    if (!fix.valid || !std::isfinite(fix.latitude) || !std::isfinite(fix.longitude) ||
        std::abs(fix.latitude) > 90 || std::abs(fix.longitude) > 180) {
        if (entry)
            *entry = {};
        return false;
    }
    if (!entry || o.at_ms - entry->last_seen > config_.max_gap_ms) {
        entry = entry ? entry : slot;
        *entry = {};
        entry->used = true;
        entry->identity = {o.radio, o.address_type, o.address};
        entry->origin = fix;
        entry->last_seen = o.at_ms;
        return false;
    }
    entry->last_seen = o.at_ms;
    constexpr double radians = 3.14159265358979323846 / 180.0;
    double lat = (fix.latitude - entry->origin.latitude) * radians;
    double lon = (fix.longitude - entry->origin.longitude) * radians;
    double a = std::sin(lat / 2) * std::sin(lat / 2) +
               std::cos(fix.latitude * radians) * std::cos(entry->origin.latitude * radians) *
                   std::sin(lon / 2) * std::sin(lon / 2);
    distance_m = 6371000.0 * 2 * std::asin(std::sqrt(std::clamp(a, 0.0, 1.0)));
    if (distance_m < config_.distance_m) {
        entry->waiting = false;
        return false;
    }
    if (!entry->waiting) {
        entry->waiting = true;
        entry->beyond_since = o.at_ms;
    }
    if (!entry->alerted && o.at_ms - entry->beyond_since >= config_.delay_ms) {
        entry->alerted = true;
        return true;
    }
    return false;
}

namespace {
bool hex(std::string_view text, uint8_t *out, size_t &size, size_t capacity) {
    size = 0;
    int high = -1;
    for (char c : text) {
        if (c == ':' || c == '-' || c == ' ')
            continue;
        int n = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        if (n < 0)
            return false;
        if (high < 0)
            high = n;
        else {
            if (size == capacity)
                return false;
            out[size++] = uint8_t(high * 16 + n);
            high = -1;
        }
    }
    return high < 0 && size > 0;
}
void text_copy(std::array<char, 33> &out, const uint8_t *src, size_t n) {
    out.fill(0);
    n = std::min(n, out.size() - 1);
    for (size_t i = 0; i < n; ++i)
        out[i] = src[i] >= 32 && src[i] < 127 ? char(src[i]) : '?';
}
bool starts(std::string_view a, std::string_view b) {
    return !b.empty() && a.size() >= b.size() && a.substr(0, b.size()) == b;
}
bool predicate(const Predicate &p, const Observation &o) {
    uint8_t bytes[32]{};
    size_t size{};
    switch (p.field) {
    case Field::ssid_prefix:
        return starts(o.ssid.data(), p.value);
    case Field::name_prefix:
        return starts(o.name.data(), p.value);
    case Field::oui:
        if ((o.radio == Radio::ble && o.address_type != 0) ||
            (o.radio == Radio::wifi && (o.address[0] & 2)))
            return false;
        return hex(p.value, bytes, size, 3) && size == 3 &&
               std::equal(bytes, bytes + 3, o.address.begin());
    case Field::manufacturer_prefix:
    case Field::service_uuid16:
        if (o.radio != Radio::ble || !hex(p.value, bytes, size, sizeof(bytes)))
            return false;
        for (size_t at = 0; at < o.payload_size;) {
            size_t n = o.payload[at];
            if (!n)
                break;
            if (at + 1 + n > o.payload_size)
                return false;
            uint8_t type = o.payload[at + 1];
            const auto *data = o.payload.data() + at + 2;
            if (p.field == Field::manufacturer_prefix && type == 0xff && n - 1 >= size &&
                std::equal(bytes, bytes + size, data))
                return true;
            if (p.field == Field::service_uuid16 && size == 2 &&
                (type == 2 || type == 3 || type == 0x16)) {
                size_t limit = type == 0x16 ? std::min<size_t>(2, n - 1) : n - 1;
                for (size_t i = 0; i + 1 < limit; i += 2)
                    if (data[i] == bytes[1] && data[i + 1] == bytes[0])
                        return true;
            }
            at += 1 + n;
        }
        return false;
    }
    return false;
}
uint16_t get16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
uint32_t get32(const uint8_t *p) {
    return uint32_t(get16(p)) | uint32_t(get16(p + 2)) << 16;
}
void put16(uint8_t *p, uint16_t n) {
    p[0] = uint8_t(n);
    p[1] = uint8_t(n >> 8);
}
void put32(uint8_t *p, uint32_t n) {
    put16(p, uint16_t(n));
    put16(p + 2, uint16_t(n >> 16));
}
uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffff;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int j = 0; j < 8; ++j)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}
uint64_t difference(uint64_t a, uint64_t b) {
    return a > b ? a - b : b - a;
}
bool number(const char *s, double &value) {
    if (!s || !*s)
        return false;
    char *end{};
    value = std::strtod(s, &end);
    return *end == 0 && std::isfinite(value);
}
bool coordinate(const char *s, const char *direction, bool latitude, double &out) {
    double raw{};
    if (!number(s, raw) || raw < 0 || std::strlen(direction) != 1)
        return false;
    int degrees = int(raw / 100);
    double minutes = raw - degrees * 100;
    if (minutes >= 60 || degrees > (latitude ? 90 : 180))
        return false;
    out = degrees + minutes / 60;
    if (out > (latitude ? 90 : 180))
        return false;
    if (*direction == (latitude ? 'S' : 'W'))
        out = -out;
    else if (*direction != (latitude ? 'N' : 'E'))
        return false;
    return true;
}
int64_t days_since_epoch(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned y = unsigned(year - era * 400);
    const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    return int64_t(era) * 146097 + int64_t(y * 365 + y / 4 - y / 100 + doy) - 719468;
}
bool utc_value(const char *date, const char *time, int64_t &out) {
    if (std::strlen(date) != 6 || std::strlen(time) < 6)
        return false;
    for (int i = 0; i < 6; ++i)
        if (date[i] < '0' || date[i] > '9' || time[i] < '0' || time[i] > '9')
            return false;
    auto two = [](const char *p) { return (p[0] - '0') * 10 + p[1] - '0'; };
    int day = two(date), month = two(date + 2), year = 2000 + two(date + 4);
    int hour = two(time), minute = two(time + 2);
    double seconds{};
    if (!number(time + 4, seconds) || hour > 23 || minute > 59 || seconds < 0 || seconds >= 60 ||
        month < 1 || month > 12)
        return false;
    const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int limit =
        lengths[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    if (day < 1 || day > limit)
        return false;
    out = (days_since_epoch(year, unsigned(month), unsigned(day)) * 86400 + hour * 3600 +
           minute * 60) *
              1000 +
          int64_t(std::llround(seconds * 1000));
    return true;
}
struct Json {
    char *out;
    size_t capacity, used{};
    bool ok{true};
    void add(const char *format, ...) {
        if (!ok)
            return;
        va_list args;
        va_start(args, format);
        int n = std::vsnprintf(out + used, capacity - used, format, args);
        va_end(args);
        if (n < 0 || size_t(n) >= capacity - used) {
            ok = false;
            return;
        }
        used += size_t(n);
    }
    void string(std::string_view s) {
        add("\"");
        for (unsigned char c : s) {
            if (c == '"' || c == '\\')
                add("\\%c", c);
            else if (c < 32)
                add("\\u%04x", unsigned(c));
            else
                add("%c", c);
        }
        add("\"");
    }
    void fix(Fix f) {
        if (!f.valid)
            add("null");
        else
            add("{\"latitude\":%.7f,\"longitude\":%.7f,\"fix_at_ms\":%llu}", f.latitude,
                f.longitude, (unsigned long long)f.at_ms);
    }
};
} // namespace

bool parse_wifi(const uint8_t *frame, size_t length, Observation &out) {
    // Beacon and probe response management headers + fixed parameters.
    if (!frame || length < 36 || ((frame[0] & 0xfc) != 0x80 && (frame[0] & 0xfc) != 0x50) ||
        (frame[0] & 3))
        return false;
    out.radio = Radio::wifi;
    out.address_type = 0;
    std::copy(frame + 10, frame + 16, out.address.begin());
    out.ssid.fill(0);
    for (size_t at = 36; at < length;) {
        if (at + 2 > length)
            return false;
        size_t n = frame[at + 1];
        if (at + 2 + n > length)
            return false;
        if (frame[at] == 0) {
            if (n > 32)
                return false;
            text_copy(out.ssid, frame + at + 2, n);
        }
        at += 2 + n;
    }
    return true;
}
bool parse_ble(const uint8_t *data, size_t length, Observation &out) {
    if (!data || length > max_payload)
        return false;
    out.radio = Radio::ble;
    out.payload_size = uint16_t(length);
    out.name.fill(0);
    std::copy(data, data + length, out.payload.begin());
    bool complete = false;
    for (size_t at = 0; at < length;) {
        size_t n = data[at];
        if (!n)
            break;
        if (at + 1 + n > length)
            return false;
        if (data[at + 1] == 9 || (data[at + 1] == 8 && !complete)) {
            text_copy(out.name, data + at + 2, n - 1);
            complete = data[at + 1] == 9;
        }
        at += n + 1;
    }
    return true;
}
bool matches(const Rule &r, const Observation &o) {
    if (!r.enabled || r.radio != o.radio || !r.predicate_count ||
        r.predicate_count > r.predicates.size())
        return false;
    for (size_t i = 0; i < r.predicate_count; ++i) {
        bool hit = predicate(r.predicates[i], o);
        if (r.require_all && !hit)
            return false;
        if (!r.require_all && hit)
            return true;
    }
    return r.require_all;
}
Match best_match(const Rule *rules, size_t count, const Observation &o) {
    Match best{};
    for (size_t i = 0; i < count; ++i)
        if (matches(rules[i], o) && (!best.rule || rules[i].priority > best.rule->priority))
            best.rule = &rules[i];
    return best;
}
bool Membership::seen(Node node, uint64_t now) {
    if (!node.id)
        return false;
    Node *slot = nullptr;
    for (auto &n : nodes_)
        if (n.id == node.id) {
            slot = &n;
            break;
        }
    if (!slot)
        for (auto &n : nodes_)
            if (!n.online) {
                slot = &n;
                break;
            }
    if (!slot)
        return false;
    bool changed = !slot->online || slot->id != node.id || slot->boot != node.boot ||
                   slot->role != node.role || slot->band24 != node.band24 ||
                   slot->band5 != node.band5;
    node.online = true;
    node.last_seen = now;
    *slot = node;
    return changed;
}
bool Membership::expire(uint64_t now, uint32_t lease) {
    bool changed = false;
    for (auto &n : nodes_)
        if (n.online && now >= n.last_seen && now - n.last_seen >= lease) {
            n.online = false;
            changed = true;
        }
    return changed;
}
Plan Membership::plan(const Channel *channels, size_t count) const {
    Plan p{};
    std::array<Node, max_nodes> sorted = nodes_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Node &a, const Node &b) { return a.id < b.id; });
    for (const auto &n : sorted)
        if (n.online && n.role == Role::child)
            p.assignments[p.count++].node = n.id;
    // Constrained channels first avoids wasting scarce band-specific capacity.
    std::array<Channel, max_channels> work{};
    count = std::min(count, max_channels);
    std::copy(channels, channels + count, work.begin());
    auto capable = [&](uint16_t id, Channel c) {
        for (const auto &n : nodes_)
            if (n.id == id)
                return c.number <= 14 ? n.band24 : n.band5;
        return false;
    };
    auto eligible = [&](Channel c) {
        size_t k = 0;
        for (size_t i = 0; i < p.count; ++i)
            k += capable(p.assignments[i].node, c);
        return k;
    };
    // Stable insertion sort is bounded to 32 channels and never allocates.
    for (size_t i = 1; i < count; ++i) {
        Channel value = work[i];
        size_t j = i;
        while (j && eligible(value) < eligible(work[j - 1])) {
            work[j] = work[j - 1];
            --j;
        }
        work[j] = value;
    }
    for (size_t c = 0; c < count; ++c) {
        size_t best = p.count;
        for (size_t i = 0; i < p.count; ++i)
            if (capable(p.assignments[i].node, work[c]) &&
                (best == p.count || p.assignments[i].count < p.assignments[best].count))
                best = i;
        if (best == p.count)
            p.uncovered[p.uncovered_count++] = work[c];
        else {
            auto &a = p.assignments[best];
            a.channels[a.count++] = work[c];
        }
    }
    return p;
}
bool Gps::feed(char byte, uint64_t now) {
    if (byte == '$') {
        length_ = 0;
        collecting_ = true;
        return false;
    }
    if (!collecting_)
        return false;
    if (byte == '\r')
        return false;
    if (byte == '\n') {
        collecting_ = false;
        line_[length_] = 0;
        return sentence(now);
    }
    if (length_ + 1 >= line_.size()) {
        collecting_ = false;
        length_ = 0;
        return false;
    }
    line_[length_++] = byte;
    return false;
}
bool Gps::sentence(uint64_t now) {
    char *star = std::strchr(line_.data(), '*');
    if (!star || std::strlen(star + 1) != 2)
        return false;
    uint8_t expected[1]{};
    size_t size{};
    if (!hex(star + 1, expected, size, 1))
        return false;
    uint8_t check = 0;
    for (char *p = line_.data(); p < star; ++p)
        check ^= uint8_t(*p);
    if (check != expected[0])
        return false;
    *star = 0;
    std::array<char *, 20> fields{};
    size_t count = 1;
    fields[0] = line_.data();
    for (char *p = line_.data(); *p; ++p)
        if (*p == ',') {
            *p = 0;
            if (count == fields.size())
                return false;
            fields[count++] = p + 1;
        }
    if (std::strlen(fields[0]) != 5 || std::strcmp(fields[0] + 2, "RMC") || count < 10 ||
        std::strcmp(fields[2], "A"))
        return false;
    int64_t epoch{};
    if (!utc_value(fields[9], fields[1], epoch))
        return false;
    has_time_ = true;
    time_at_ = now;
    utc_at_ = epoch;
    Fix fix{};
    fix.at_ms = now;
    fix.valid = coordinate(fields[3], fields[4], true, fix.latitude) &&
                coordinate(fields[5], fields[6], false, fix.longitude);
    if (fix.valid) {
        fixes_[next_fix_] = fix;
        next_fix_ = (next_fix_ + 1) % fixes_.size();
    }
    return true;
}
Fix Gps::nearest(uint64_t capture, uint32_t max_age) const {
    Fix best{};
    uint64_t delta = UINT64_MAX;
    for (auto f : fixes_)
        if (f.valid) {
            uint64_t d = difference(capture, f.at_ms);
            if (d <= max_age && d < delta) {
                best = f;
                delta = d;
            }
        }
    return best;
}
Fix Gps::tag(const Observation &o, uint32_t max_age, uint32_t max_uncertainty) const {
    if (o.timing_uncertainty_ms > max_uncertainty || o.timing_uncertainty_ms > max_age)
        return {};
    // The entire possible capture-time interval must be within the fix-age limit.
    return nearest(o.at_ms, max_age - o.timing_uncertainty_ms);
}
bool Gps::utc(uint64_t at, int64_t &out) const {
    if (!has_time_)
        return false;
    // Avoid unsigned wrap when associating buffered observations before last sync.
    out = utc_at_ + (at >= time_at_ ? int64_t(at - time_at_) : -int64_t(time_at_ - at));
    return true;
}
bool same_signal(const Observation &a, const Observation &b) {
    return a.node == b.node && a.boot == b.boot && a.radio == b.radio &&
           a.address_type == b.address_type && a.address == b.address && a.channel == b.channel;
}
Track &Tracker::observe(const Observation &o, Fix fix) {
    Track *t = nullptr;
    for (auto &x : tracks_)
        if (x.used && same_signal(x.latest, o)) {
            t = &x;
            break;
        }
    if (!t)
        for (auto &x : tracks_)
            if (!x.used) {
                t = &x;
                break;
            }
    if (!t)
        t = &*std::min_element(tracks_.begin(), tracks_.end(), [](const Track &a, const Track &b) {
            return a.latest.at_ms < b.latest.at_ms;
        });
    if (!t->used || !same_signal(t->latest, o)) {
        *t = {};
        t->used = true;
        t->first_seen = o.at_ms;
        t->window_start = o.at_ms;
        t->peak = o;
        t->peak_fix = fix;
        t->peak_changed = true;
    } else
        t->peak_changed = false;
    if (o.at_ms < t->latest.at_ms)
        return *t; // stale/out-of-order sample
    if (t->count && o.at_ms - t->latest.at_ms > config_.stale_ms) {
        t->sample_count = 0;
        t->rssi_histogram.fill(0);
        t->previous_valid = false;
        t->trend = Trend::unknown;
        t->candidate = Trend::unknown;
        t->confirmations = 0;
        t->window_start = o.at_ms;
    }
    if (o.rssi > t->peak.rssi) {
        t->peak = o;
        t->peak_fix = fix;
        t->peak_changed = true;
    }
    if (config_.window_ms && o.at_ms - t->window_start >= config_.window_ms) {
        if (t->sample_count >= config_.minimum_samples && config_.minimum_samples) {
            size_t accumulated = 0;
            int median = -128;
            for (size_t i = 0; i < t->rssi_histogram.size(); ++i) {
                accumulated += t->rssi_histogram[i];
                if (accumulated > t->sample_count / 2) {
                    median = int(i) - 127;
                    break;
                }
            }
            if (t->previous_valid) {
                int delta = median - t->previous_median;
                Trend next = delta > config_.deadband_db    ? Trend::increasing
                             : delta < -config_.deadband_db ? Trend::decreasing
                                                            : Trend::steady;
                if (next == t->candidate) {
                    if (t->confirmations < 255)
                        ++t->confirmations;
                } else {
                    t->candidate = next;
                    t->confirmations = 1;
                }
                if (t->confirmations >= config_.confirmations)
                    t->trend = next;
            }
            t->previous_valid = true;
            t->previous_median = median;
        } else {
            t->previous_valid = false;
            t->trend = Trend::unknown;
            t->candidate = Trend::unknown;
            t->confirmations = 0;
        }
        t->sample_count = 0;
        t->rssi_histogram.fill(0);
        t->window_start = o.at_ms;
    }
    if (o.rssi >= -127 && o.rssi <= 0 && t->sample_count < UINT32_MAX) {
        ++t->rssi_histogram[size_t(int(o.rssi) + 127)];
        ++t->sample_count;
    }
    t->latest = o;
    ++t->count;
    return *t;
}
void Tracker::reset_trend(Track &t) {
    t.sample_count = 1;
    t.rssi_histogram.fill(0);
    ++t.rssi_histogram[size_t(std::clamp(int(t.latest.rssi), -127, 0) + 127)];
    t.window_start = t.latest.at_ms;
    t.previous_valid = false;
    t.trend = t.candidate = Trend::unknown;
    t.confirmations = 0;
}
Trend Tracker::trend(const Track &t, uint64_t now) const {
    return !t.used || now < t.latest.at_ms || now - t.latest.at_ms > config_.stale_ms
               ? Trend::unknown
               : t.trend;
}
const char *trend_name(Trend t) {
    switch (t) {
    case Trend::increasing:
        return "increasing";
    case Trend::decreasing:
        return "decreasing";
    case Trend::steady:
        return "steady";
    default:
        return "unknown";
    }
}

size_t encode_packet(const Packet &p, uint8_t *out, size_t cap) {
    size_t n = 20 + p.size;
    if (p.size > p.payload.size() || cap < n)
        return 0;
    out[0] = 0x57;
    out[1] = 0x4b;
    out[2] = 1;
    out[3] = uint8_t(p.type);
    put16(out + 4, p.node);
    put32(out + 6, p.boot);
    put32(out + 10, p.sequence);
    put16(out + 14, p.size);
    std::copy(p.payload.begin(), p.payload.begin() + p.size, out + 16);
    put32(out + 16 + p.size, crc(out + 2, 14 + p.size));
    return n;
}
bool Decoder::feed(uint8_t byte, Packet &out) {
    if (size_ == 0 && byte != 0x57)
        return false;
    if (size_ == 1 && byte != 0x4b) {
        size_ = byte == 0x57 ? 1 : 0;
        return false;
    }
    data_[size_++] = byte;
    if (size_ < 16)
        return false;
    size_t payload = get16(data_.data() + 14);
    if (data_[2] != 1 || data_[3] < 1 || data_[3] > 2 || payload > 512) {
        ++errors_;
        size_ = 0;
        return false;
    }
    if (size_ < 20 + payload)
        return false;
    size_ = 0;
    if (get32(data_.data() + 16 + payload) != crc(data_.data() + 2, 14 + payload)) {
        ++errors_;
        return false;
    }
    out.type = MessageType(data_[3]);
    out.node = get16(data_.data() + 4);
    out.boot = get32(data_.data() + 6);
    out.sequence = get32(data_.data() + 10);
    out.size = uint16_t(payload);
    std::copy(data_.begin() + 16, data_.begin() + 16 + payload, out.payload.begin());
    return true;
}
bool pack_command(const Command &c, Packet &p) {
    if (c.count > max_channels)
        return false;
    p.type = MessageType::poll;
    p.size = uint16_t(19 + c.count);
    auto *b = p.payload.data();
    put32(b, c.lease_ms);
    put32(b + 4, c.dwell_ms);
    b[8] = c.count;
    for (size_t i = 0; i < c.count; ++i)
        b[9 + i] = c.channels[i].number;
    size_t at = 9 + c.count;
    b[at++] = c.follow;
    b[at++] = uint8_t(c.target_radio);
    b[at++] = c.target_channel;
    b[at++] = c.target_address_type;
    std::copy(c.target.begin(), c.target.end(), b + at);
    return true;
}
bool unpack_command(const Packet &p, Command &c) {
    if (p.type != MessageType::poll || p.size < 19)
        return false;
    const auto *b = p.payload.data();
    uint8_t n = b[8];
    if (n > max_channels || p.size != 19 + n || get32(b) < 100 || get32(b) > 60000 ||
        get32(b + 4) < 10 || get32(b + 4) > 60000)
        return false;
    c = {};
    c.lease_ms = get32(b);
    c.dwell_ms = get32(b + 4);
    c.count = n;
    for (size_t i = 0; i < n; ++i) {
        if (!b[9 + i])
            return false;
        c.channels[i].number = b[9 + i];
    }
    size_t at = 9 + n;
    if (b[at] > 1 || b[at + 1] > 1 || b[at + 3] > 1)
        return false;
    c.follow = b[at++];
    c.target_radio = Radio(b[at++]);
    c.target_channel = b[at++];
    c.target_address_type = b[at++];
    std::copy(b + at, b + at + 6, c.target.begin());
    return true;
}
bool pack_report(const Report &r, Packet &p) {
    if (r.observation.payload_size > max_payload)
        return false;
    p.type = MessageType::report;
    auto *b = p.payload.data();
    b[0] = uint8_t(r.role);
    b[1] = r.band24;
    b[2] = r.band5;
    b[3] = r.has_observation;
    put32(b + 4, r.dropped);
    p.size = 8;
    if (!r.has_observation)
        return true;
    const auto &o = r.observation;
    put32(b + 8, r.capture_age_ms);
    put32(b + 12, o.sequence);
    b[16] = uint8_t(o.radio);
    b[17] = o.address_type;
    b[18] = o.channel;
    b[19] = uint8_t(o.rssi);
    std::copy(o.address.begin(), o.address.end(), b + 20);
    std::memcpy(b + 26, o.ssid.data(), 33);
    std::memcpy(b + 59, o.name.data(), 33);
    put16(b + 92, o.payload_size);
    std::copy(o.payload.begin(), o.payload.begin() + o.payload_size, b + 94);
    p.size = uint16_t(94 + o.payload_size);
    return true;
}
bool unpack_report(const Packet &p, Report &r) {
    if (p.type != MessageType::report || p.size < 8)
        return false;
    const auto *b = p.payload.data();
    if (b[0] < 1 || b[0] > 2 || b[1] > 1 || b[2] > 1 || b[3] > 1)
        return false;
    r = {};
    r.role = Role(b[0]);
    r.band24 = b[1];
    r.band5 = b[2];
    r.has_observation = b[3];
    r.dropped = get32(b + 4);
    if (!r.has_observation)
        return p.size == 8;
    if (p.size < 94 || get16(b + 92) > max_payload || p.size != 94 + get16(b + 92) || b[16] > 1 ||
        b[17] > 1 || b[58] != 0 || b[91] != 0)
        return false;
    auto &o = r.observation;
    o.node = p.node;
    o.boot = p.boot;
    r.capture_age_ms = get32(b + 8);
    o.sequence = get32(b + 12);
    o.radio = Radio(b[16]);
    o.address_type = b[17];
    o.channel = b[18];
    o.rssi = int8_t(b[19]);
    if (o.rssi > 0 || o.rssi < -127)
        return false;
    std::copy(b + 20, b + 26, o.address.begin());
    std::memcpy(o.ssid.data(), b + 26, 33);
    std::memcpy(o.name.data(), b + 59, 33);
    o.payload_size = get16(b + 92);
    std::copy(b + 94, b + 94 + o.payload_size, o.payload.begin());
    return true;
}
bool observation_json(const Observation &o, const Track &t, Match match, Fix fix, const Gps &gps,
                      char *out, size_t cap) {
    if (!out || !cap)
        return false;
    Json j{out, cap};
    int64_t utc{};
    j.add("{\"schema_version\":1,\"node\":%u,\"boot\":%u,\"sequence\":%u,\"capture_ms\":%llu,\"utc_"
          "ms\":",
          unsigned(o.node), unsigned(o.boot), unsigned(o.sequence), (unsigned long long)o.at_ms);
    if (gps.utc(o.at_ms, utc))
        j.add("%lld", (long long)utc);
    else
        j.add("null");
    j.add(",\"time_source\":\"%s\",\"gps_sync_age_ms\":",
          gps.has_time() ? "gps_holdover" : "unknown");
    if (gps.has_time())
        j.add("%llu", (unsigned long long)difference(o.at_ms, gps.last_sync()));
    else
        j.add("null");
    j.add(",\"timing_uncertainty_ms\":%u,\"radio\":\"%s\",\"channel\":%u,\"address_type\":%u,"
          "\"address\":\"",
          unsigned(o.timing_uncertainty_ms), o.radio == Radio::wifi ? "wifi" : "ble",
          unsigned(o.channel), unsigned(o.address_type));
    for (size_t i = 0; i < 6; ++i)
        j.add("%s%02x", i ? ":" : "", unsigned(o.address[i]));
    j.add("\",\"rssi_dbm\":%d,\"ssid\":", int(o.rssi));
    j.string(o.ssid.data());
    j.add(",\"name\":");
    j.string(o.name.data());
    j.add(",\"rule_id\":");
    if (match.rule)
        j.string(match.rule->id);
    else
        j.add("null");
    j.add(",\"receiver_boot\":%u,\"confidence\":", unsigned(o.receiver_boot));
    if (match.rule)
        j.string(match.rule->confidence);
    else
        j.add("null");
    j.add(",\"ble_ad_hex\":\"");
    if (o.radio == Radio::ble)
        for (size_t i = 0; i < std::min<size_t>(o.payload_size, o.payload.size()); ++i)
            j.add("%02x", unsigned(o.payload[i]));
    j.add("\"");
    j.add(",\"receiver_fix\":");
    j.fix(fix);
    j.add(",\"peak_dbm\":%d,\"peak_capture_ms\":%llu,\"peak_receiver_fix\":", int(t.peak.rssi),
          (unsigned long long)t.peak.at_ms);
    j.fix(t.peak_fix);
    j.add(",\"peak_changed\":%s,\"observations\":%llu}\n", t.peak_changed ? "true" : "false",
          (unsigned long long)t.count);
    return j.ok;
}
} // namespace wiknow
