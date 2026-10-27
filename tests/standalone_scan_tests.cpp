#include "wiknow/standalone_scan.hpp"
#include <cassert>

int main() {
    wiknow::StandaloneScan scan;
    uint64_t now = 0;
    for (size_t i = 0; i < 9; ++i) {
        assert(scan.due(now));
        assert(!scan.ble() && scan.channel_index() == i % 3);
        scan.started(now, 120, 300);
        assert(!scan.due(now + 119));
        assert(scan.due(now + 120));
        scan.advance(true, 3);
        assert(scan.ble() && scan.channel_index() == i % 3);
        now += 120;
        scan.started(now, 120, 300);
        assert(!scan.due(now + 299));
        // Long processing delays must not skip channels.
        now += 10000;
        assert(scan.due(now));
        scan.advance(true, 3);
    }
    wiknow::StandaloneScan wifi_only;
    for (size_t i = 0; i < 9; ++i) {
        assert(!wifi_only.ble() && wifi_only.channel_index() == i % 3);
        wifi_only.started(i * 120, 120, 300);
        wifi_only.advance(false, 3);
    }
    wiknow::StandaloneScan one_channel;
    one_channel.started(UINT64_C(0xffffffff), 120, 300);
    assert(!one_channel.due(UINT64_C(0x100000000)));
    one_channel.advance(true, 1);
    one_channel.advance(true, 1);
    assert(!one_channel.ble() && one_channel.channel_index() == 0);
}
