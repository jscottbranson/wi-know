#include "wiknow/remote_id.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace wiknow;
using Message = std::array<uint8_t, 25>;
static Observation ble(const std::vector<uint8_t> &message) {
    std::vector<uint8_t> ad{uint8_t(message.size() + 5), 0x16, 0xfa, 0xff, 0x0d, 7};
    ad.insert(ad.end(), message.begin(), message.end());
    Observation o{};
    assert(parse_ble(ad.data(), ad.size(), o));
    return o;
}
static Observation ble(const Message &message) {
    return ble(std::vector<uint8_t>(message.begin(), message.end()));
}
static void put32(Message &m, size_t at, uint32_t v) {
    for (size_t i = 0; i < 4; ++i)
        m[at + i] = uint8_t(v >> (i * 8));
}
static std::string json(const Observation &o, size_t index = 0) {
    char output[log_line_capacity];
    Gps gps;
    assert(drone_detection_json(o, index, {}, gps, output, sizeof(output)));
    std::cout << output;
    return output;
}
int main() {
    Message basic{};
    basic[0] = 0x02;
    basic[1] = 0x12;
    std::memcpy(basic.data() + 2, "DRONE-123", 9);
    auto o = ble(basic);
    assert(remote_id_messages(o).count == 1);
    assert(json(o).find("\"uas_id\":\"DRONE-123\"") != std::string::npos);
    assert(json(o).find("\"message_counter\":7") != std::string::npos);
    Message loc{};
    loc[0] = 0x12;
    loc[1] = 0x23; // airborne, west direction, high speed multiplier
    loc[2] = 90;
    loc[3] = 100;
    loc[4] = 252; // -2 m/s
    put32(loc, 5, 421234567);
    put32(loc, 9, uint32_t(-831234567));
    loc[15] = 0x98;
    loc[16] = 0x08; // 100 m geodetic
    loc[21] = 123;  // 12.3 seconds after hour
    auto location = json(ble(loc));
    for (auto expected : {"\"latitude\":42.1234567", "\"longitude\":-83.1234567",
                          "\"speed_horizontal_mps\":138.7500000",
                          "\"speed_vertical_mps\":-2.0000000", "\"direction_deg\":270.0000000",
                          "\"altitude_geodetic_m\":100.0000000", "\"altitude_baro_m\":null"})
        assert(location.find(expected) != std::string::npos);
    Message unknown{};
    unknown[0] = 0x12;
    unknown[1] = 3;
    unknown[2] = 181;
    unknown[3] = 255;
    unknown[4] = 126;
    unknown[21] = unknown[22] = 255;
    auto nulls = json(ble(unknown));
    for (auto key : {"latitude", "longitude", "direction_deg", "speed_horizontal_mps",
                     "speed_vertical_mps", "seconds_after_hour"})
        assert(nulls.find(std::string("\"") + key + "\":null") != std::string::npos);
    put32(unknown, 5, 910000000);
    assert(json(ble(unknown)).find("\"latitude\":null") != std::string::npos);
    std::vector<uint8_t> pack{0xf2, 25, 2};
    pack.insert(pack.end(), basic.begin(), basic.end());
    pack.insert(pack.end(), loc.begin(), loc.end());
    o = ble(pack);
    assert(remote_id_messages(o).count == 2);
    assert(json(o, 1).find("\"message_type\":1") != std::string::npos);
    // Wi-Fi beacon vendor IE -> bus -> parent -> decoded storage.
    std::vector<uint8_t> frame(36, 0);
    frame[0] = 0x80;
    frame.insert(frame.end(), {221, uint8_t(pack.size() + 5), 0xfa, 0x0b, 0xbc, 0x0d, 8});
    frame.insert(frame.end(), pack.begin(), pack.end());
    assert(parse_wifi(frame.data(), frame.size(), o));
    assert(remote_id_messages(o).count == 2);
    Report report{};
    report.has_observation = true;
    report.observation = o;
    Packet packet{};
    assert(pack_report(report, packet));
    Report decoded{};
    assert(unpack_report(packet, decoded));
    assert(remote_id_messages(decoded.observation).count == 2);
    assert(json(decoded.observation, 1).find("\"radio\":\"wifi\"") != std::string::npos);
    char output[log_line_capacity];
    Tracker tracker;
    assert(observation_json(o, tracker.observe(o, {}), {}, {}, Gps{}, output, sizeof(output)));
    assert(std::strstr(output, "\"wifi_remote_id_hex\":\"fa0bbc0d08f2"));
    std::cout << output;
    // Reused parse output must not retain an earlier drone payload.
    assert(parse_wifi(frame.data(), 36, o));
    assert(remote_id_messages(o).count == 0);
    for (size_t n = 0; n < pack.size(); ++n) {
        std::vector<uint8_t> truncated(pack.begin(), pack.begin() + n);
        assert(remote_id_messages(ble(truncated)).count == 0);
    }
    for (size_t at : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(28)}) {
        auto bad = pack;
        bad[at] = 0xff;
        assert(remote_id_messages(ble(bad)).count == 0);
    }
    o = ble(basic);
    o.payload[2] = 0xfb;
    assert(!remote_id_messages(o).count);
    o = ble(basic);
    o.payload_size = 256;
    assert(!remote_id_messages(o).count);
    basic[0] = 0x03;
    assert(!remote_id_messages(ble(basic)).count);
    // Other available messages and binary/escaped strings, worst-case length.
    for (uint8_t type : {0, 2, 3, 4, 5}) {
        Message message{};
        message.fill(0xff);
        message[0] = uint8_t((type << 4) | 2);
        message[1] = type == 0 ? 0x12 : 0;
        o = ble(message);
        assert(remote_id_messages(o).count == 1);
        json(o);
        assert(!drone_detection_json(o, 1, {}, Gps{}, output, sizeof(output)));
        assert(!drone_detection_json(o, 0, {}, Gps{}, output, 16));
    }
    Message system{};
    system[0] = 0x42;
    system[1] = 5;
    put32(system, 2, uint32_t(-338765432));
    put32(system, 6, 1511234567);
    system[10] = 2;
    system[12] = 7;
    system[18] = 0x34;
    system[19] = 0x08; // 50 m HAE
    put32(system, 20, 123456789);
    auto system_json = json(ble(system));
    for (auto expected :
         {"\"operator_position\":{\"latitude\":-33.8765432", "\"longitude\":151.1234567",
          "\"area_count\":2", "\"area_radius_m\":70", "\"operator_altitude_geodetic_m\":50.0000000",
          "\"timestamp_since_2019_s\":123456789"})
        assert(system_json.find(expected) != std::string::npos);
    // A truncated later AD invalidates the entire envelope, not just its suffix.
    o = ble(loc);
    o.payload[o.payload_size++] = 8;
    assert(!remote_id_messages(o).count);
    frame[38] = 0xfb; // unrelated vendor OUI
    assert(parse_wifi(frame.data(), frame.size(), o));
    assert(!remote_id_messages(o).count);
    // Largest supported pack is retained whole and each record fits the log queue.
    pack = {0xf2, 25, 9};
    for (size_t i = 0; i < 9; ++i)
        pack.insert(pack.end(), loc.begin(), loc.end());
    o = ble(pack);
    assert(remote_id_messages(o).count == 9);
    for (size_t i = 0; i < 9; ++i)
        json(o, i);
}
