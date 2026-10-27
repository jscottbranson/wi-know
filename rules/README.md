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
| `priority` | 0–255; highest matching priority determines actions and target selection |
| `confidence` | `possible`, `probable`, or `broadcast`; a declared evidence label, not authentication |
| `actions` | Boolean `buzzer` and `follow` switches |
| `predicates` | One to four field/value objects |
| `source` | Nonempty `url`, `revision`, `license`, and `reviewed` strings |

Supported predicates:

| `field` | `value` format |
| --- | --- |
| `oui` | Three address bytes, e.g. `00:11:22`; ignored for local/random addresses |
| `ssid_prefix` | Case-sensitive UTF-8 Wi-Fi SSID prefix, at most 32 encoded bytes |
| `name_prefix` | Case-sensitive UTF-8 BLE advertised name prefix, at most 32 encoded bytes |
| `manufacturer_prefix` | Hex prefix of BLE AD type `0xff`, including little-endian company ID bytes |
| `service_uuid16` | Conventional 16-bit UUID text, e.g. `180F`; compared against little-endian AD bytes |

Only Wi-Fi beacons/probe responses and complete BLE advertising reports up to
255 bytes are parsed initially. Text matching retains up to 32 received bytes of
the name/SSID, stopping at the first NUL byte. Prefixes match those bytes exactly,
including UTF-8; they cannot contain ASCII control characters or exceed the
32-byte limit. Regex is not supported. Match evidence is retained as SSID/name,
`ssid_hex` for that captured text prefix, and BLE advertising hex in
JSONL. Every matching rule produces a classification record with its category,
confidence and priority; records for the same capture share the same observation
identity and count. The highest-priority match controls alerts and target selection.
OUI matches indicate a possible vendor association, not confirmed equipment identity.

Every build also writes `settings.rules.json` beside its generated `settings.hpp`.
This manifest preserves all compiled rules, their predicates, confidence, actions,
source URL/revision/license/review date, and enabled state after category filtering.
Share it alongside observation logs to explain the signatures behind each rule ID.
Keep the manifest from the same build as the observations; a later rebuild can
change a rule while retaining its ID.

Potential upstream sources: [Eye Spy](https://github.com/simeononsecurity/eye-spy),
[OUI Spy](https://github.com/colonelpanichacks/oui-spy-unified-blue), and
[OpenDroneID](https://github.com/opendroneid/opendroneid-core-c). Review licensing,
fix the exact revision, and retain attribution when importing. Enable Flock, Axon,
ALPR, Ring, or drone categories only with reviewed signatures appropriate to the
radio/parser. Category switches alone do not add signatures. Drone telemetry
payload decoding is not implemented in this first version.
