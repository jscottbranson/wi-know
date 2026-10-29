# Optional connected BLE primary-service discovery

Active GATT discovery is a new, experimental software capability for the existing
ESP32-C5. It transmits and connects; the target can observe it. All shipped
configuration profiles disable it. Passive advertising reception remains the
default. No pairing, bonding, credential attempts, characteristic reads or writes,
notifications, or secondary-service discovery are performed.

## Source review and limits

The reviewed sources are Eye Spy revision
`5dba436175fd670a67eb819c2e17407d6fe918e8`, `src/main.cpp` around its Raven and Flock
service checks, and OUI Spy revision
`46eb13b2746ae72ad7070bbf5e42deb09b573623`,
`src/raw/flockyou_promiscious.cpp` around `fyBleMatch`. Both inspect advertised
service UUIDs, despite calling those identifiers GATT services. Neither provides
an active discovery workflow to reproduce. Their licenses and notices are retained
in [rules/sources](../rules/sources/README.md); no implementation code is imported.

The Flock accessory identifier `e8ccbb38-9532-46a8-9fe5-1814df172e6f` and Raven
16-bit service range are candidates for future controlled research. These sources
do not establish that a particular device exposes them only after connection.
This change adds no production connected-service signatures and makes no vendor
parity claim. Generic standard services and Nordic DFU are not vendor identity.
A custom rule must have reviewed provenance; the included fixture is synthetic.

The SDK audited is ESP-IDF v5.5.1 and its pinned NimBLE:
`host/include/host/ble_gap.h`, `ble_gatt.h`, `host/src/ble_gap.c`, and `ble_sm.c`.
`ble_gap_conn_cancel()` acknowledges the controller command before delivering its
CONNECT cancellation callback. The application keeps the attempt occupied until
that callback. A successful connection uses `ble_gattc_disc_all_svcs()` and only
`BLE_HS_EDONE` publishes the complete primary-service list. With BLE security
compiled out, the `!MYNEWT_VAL(BLE_SM)` branch in `ble_sm.c` responds with
`BLE_SM_ERR_PAIR_NOT_SUPP`. The build rejects enabled GATT if security is compiled
in, including custom SDK configurations: Just Works pairing must not slip through
without a passkey callback.

## Enable deliberately

Use a fresh SDK configuration and append `firmware/sdkconfig.gatt.defaults` after
`firmware/sdkconfig.defaults`. The overlay enables one central connection and one
GATT procedure, reserves one controller connection, and disables security/bonding.
The default SDK profile retains central disabled and zero reserved connections.
An enabled JSON setting with central disabled fails the firmware build explicitly.

```sh
idf.py -C firmware -B build-gatt -DIDF_TARGET=esp32c5 \
  -DSDKCONFIG="$PWD/build-gatt/sdkconfig" \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.gatt.defaults' \
  -DWIKNOW_CONFIG="$PWD/tests/fixtures/gatt/settings.json" build
```

The CI fixture is a working example for a controlled test advertiser. Replace its
address and service with your own test device before deployment. It uses no vendor
classification. For ordinary profiles add:

```json
"gatt": {
  "enabled": true,
  "timeout_ms": 3000,
  "cooldown_ms": 60000,
  "targets": [{"address_type": 0, "address": "12:34:56:78:9a:bc"}]
}
```

Each target requires a matching **enabled** BLE rule with `match: "all"`, exactly
one `mac` predicate, one or more `gatt_service_uuid128` predicates, and
`actions.follow: false`. No other predicates may be mixed into that rule. UUIDs
use canonical 8-4-4-4-12 text; a 16-bit service such as 180F is represented as
`0000180f-0000-1000-8000-00805f9b34fb`. The generator normalizes case and preserves
predicates/source/confidence in the provenance manifest.

The category must also be enabled. An allowlisted address alone, a passive rule,
or a disabled detector cannot cause a connection. Enabling GATT never changes
category switches; tracker and UniFi policies remain those of the selected
configuration. No class-wide identity correlation is attempted.

## Bounded scheduling and evidence

There are at most 16 configured addresses. Public (`address_type: 0`) and static
random (`1`, top two bits `11`) are supported; rotating private and identity-resolved
addresses are excluded. Static random addresses may change after reboot and need
explicit configuration updates. Only connectable 1M reports are candidates; coded
primary PHYs, scan responses and nonconnectable reports do not trigger discovery.

The receiving BLE node performs discovery and forwards results through the normal
observation queue/bus. Traditional Wi-Fi-only children do not start BLE. A universal
worker or follower may discover an allowed target subject to its capture assignment;
a standalone/local receiver uses the same path. There is one pending candidate and
one connection, no candidate eviction, and no immediate retries. A selected candidate
expires after one second and consumes its per-address cooldown even on cancellation
or failure. Cooldown is 60 seconds–24 hours; total connection plus discovery deadline
is 500 ms–10 seconds. Reboot clears cooldowns.

Passive scanning stops before connecting and its existing queue barrier drains old
advertisements. Scheduler transitions cancel pending work and wait for disconnection
before tuning Wi-Fi. The next passive scan begins only after the session is released.
A failed termination causes one host-reset request after two seconds; host reset
invalidates session tokens and scan evidence. Late callbacks cannot update a newer
session, including reuse of a connection handle. Busy targets, connection failures,
service errors, disconnects, timeouts, or more than 15 primary services discard the
whole result. No partial service list becomes matching evidence.

Standalone BLE dwell must allow the configured discovery deadline; a 120 ms default
window generally cancels discovery. The synthetic fixture uses a five-second BLE
window for its three-second deadline. Increasing dwell reduces Wi-Fi coverage.

Each successful result stores up to 15 canonical, big-endian 128-bit UUIDs in the
existing bounded payload. Standard 16/32-bit identifiers expand to the Bluetooth
base UUID. It is a distinct `BleEvidence::gatt_primary_services` observation, never
fabricated AD bytes. The timestamp is completion time; services were obtained during
the bounded preceding discovery interval. Address/type, node/boot/sequence and
capture age survive forwarding. RSSI is read from the live connection at completion;
if unavailable, no result is published. Connected evidence uses separate RSSI tracks,
is excluded from generic following scoring and cannot select a follower target.
Fresh matching results may use the existing shared buzzer cooldown.

JSONL identifies `ble_acquisition: "connected_primary_discovery"` and retains
`gatt_services_hex`; `ble_ad_hex` is empty. Advertising records use
`passive_advertisement`. CSV/TSV exports retain both fields. Existing matches-only
retention and rule provenance apply. Service rules never match advertising evidence,
and advertising predicates cannot inspect connected-service bytes.

Bus protocol 5 adds the BLE evidence discriminator at report offset 132; payload
starts at 133. Maximum report payload is 388 bytes (420 within a universal identity
envelope). Upgrade every bus participant together; older protocol versions are
rejected rather than interpreted as advertising.

## Resource budget and validation status

Application state is fixed: a 16-entry cooldown table, one candidate, one result and
one small lifecycle object. Discovery reuses the NimBLE host task (4096-byte configured
stack), parser task and observation queue; it adds no task or unbounded allocation.
NimBLE adds one connection, one GATT procedure and its controller state. The actual
heap cost is SDK-dependent and must be measured on the board with Wi-Fi and storage
active. Every 30 seconds the active path logs attempt/completion/failure counters,
state, internal free heap and minimum heap. Existing BLE diagnostics remain available.

Host tests cover admission, cooldown, stale candidates, random-address rejection,
category disabling, token/handle reuse, cancellation races, timeouts, reset watchdog,
wire/retention/log evidence and isolation from passive RSSI/following. CI compiles both
passive profiles and the enabled-central synthetic fixture.

**Hardware validation remains pending (issue #40 stays open).** On a controlled C5
and advertiser, verify discovered-only versus advertised-only services, generic
negatives, denied/busy targets, deliberate disconnects, timeouts, rotating addresses,
window cancellation, reset/passive-scan recovery, and heap/stack headroom with Wi-Fi
and storage. Measure disabled behavior (no connections), throughput and dwell loss.
No real-device compatibility or runtime memory margin is claimed from a build alone.
