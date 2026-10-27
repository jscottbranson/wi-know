# Detection matching rules

Place production rule files directly in this directory as `*.json`. Every such
file is validated and compiled into firmware. Subdirectories are excluded, so
`examples/` contains inactive templates. Rebuild/reflash the parent after edits.
No downloaded signature collection is bundled yet; `rules.json` starts empty.
An empty enabled ruleset produces a visible build/startup diagnostic.

For a controlled test, copy `examples/lab.json` here and set its `enabled` field
to `true`. A beacon whose SSID starts with `WI-KNOW-TEST-` will match. This is a
synthetic test signature, not a vendor fingerprint. Delete/disable it after testing.

Each file has `schema_version: 1` and a `rules` array. See the lab example for a
complete entry. Required fields:

| Field | Meaning |
| --- | --- |
| `id` | Globally unique rule identifier, at most 64 characters |
| `category` | Category enabled in `config/settings.json` |
| `enabled` | Individual rule switch |
| `radio` | `wifi` or `ble` |
| `match` | Require `all` predicates or `any` predicate |
| `priority` | 0–255; highest matching priority determines logging label/actions/target selection |
| `confidence` | `possible`, `probable`, or `broadcast`; a declared evidence label, not authentication |
| `actions` | Boolean `buzzer` and `follow` switches |
| `predicates` | One to four field/value objects |
| `source` | Nonempty `url`, `revision`, `license`, and `reviewed` strings |

Supported predicates:

| `field` | `value` format |
| --- | --- |
| `oui` | Three address bytes, e.g. `00:11:22`; ignored for local/random addresses |
| `ssid_prefix` | Case-sensitive Wi-Fi SSID prefix |
| `name_prefix` | Case-sensitive BLE advertised name prefix |
| `manufacturer_prefix` | Hex prefix of BLE AD type `0xff`, including little-endian company ID bytes |
| `service_uuid16` | Conventional 16-bit UUID text, e.g. `180F`; compared against little-endian AD bytes |

Only Wi-Fi beacons/probe responses and complete BLE advertising reports up to
255 bytes are parsed initially. Names/SSIDs are sanitized to printable ASCII;
non-ASCII signatures and regex are not supported. Match evidence is retained as
SSID/name and BLE advertising hex in JSONL, alongside the winning rule and confidence.
OUI matches indicate a possible vendor association, not confirmed equipment identity.

Potential upstream sources: [Eye Spy](https://github.com/simeononsecurity/eye-spy),
[OUI Spy](https://github.com/colonelpanichacks/oui-spy-unified-blue), and
[OpenDroneID](https://github.com/opendroneid/opendroneid-core-c). Review licensing,
fix the exact revision, and retain attribution when importing. Enable Flock, Axon,
ALPR, Ring, or drone categories only with reviewed signatures appropriate to the
radio/parser. Category switches alone do not add signatures. Drone telemetry
payload decoding is not implemented in this first version.
