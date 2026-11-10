#include "device_identity.hpp"
#include <algorithm>
#include <cassert>

static wiknow::DeviceId factory_mac;
static esp_err_t read_result;
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type) {
    assert(type == ESP_MAC_EFUSE_FACTORY);
    if (!read_result)
        std::copy(factory_mac.begin(), factory_mac.end(), mac);
    return read_result;
}

int main() {
    // These boards share the first four factory bytes. Truncating C5's expanded
    // eight-byte MAC to six bytes made all four the same discovery identity.
    wiknow::DiscoveryTable table(1, 8);
    uint32_t token = 100;
    for (uint8_t i = 0; i < 4; ++i) {
        factory_mac = {0x10, 0xbd, 0xa3, 0xc8, 0x12, i};
        struct {
            wiknow::DeviceId id{};
            uint8_t guard[2]{0xa5, 0x5a};
        } destination;
        assert(platform::read_device_identity(destination.id) == 0);
        assert(destination.id == factory_mac);
        assert(destination.guard[0] == 0xa5 && destination.guard[1] == 0x5a);
        wiknow::Identity hello;
        hello.parent = {2, 3, 4, 5, 6, 7};
        hello.worker = destination.id;
        hello.worker_boot = 1000 + i;
        auto *entry = table.offer(hello, 0, 10000, ++token);
        assert(entry && entry->node == i + 1);
        table.roles(true, token);
        assert(table.acknowledge(entry->node, entry->identity, 1, 10000));
    }
    // Joined workers may receive a role change; confirm the reassignment.
    table.roles(true, token);
    for (const auto &entry : table.nodes())
        if (entry.node && !entry.acknowledged)
            assert(table.acknowledge(entry.node, entry.identity, 2, 10000));
    unsigned children = 0, followers = 0;
    for (const auto &entry : table.nodes()) {
        if (!entry.node)
            continue;
        assert(entry.acknowledged);
        children += entry.identity.role == wiknow::Role::child;
        followers += entry.identity.role == wiknow::Role::follower;
    }
    assert(children == 3 && followers == 1);
    read_result = -1;
    wiknow::DeviceId id{};
    assert(platform::read_device_identity(id) == read_result);
}
