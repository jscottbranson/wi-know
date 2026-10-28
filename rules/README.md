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
| `rid_valid` | String `"true"`: at least one structurally valid supported decoded Remote ID message |
| `rid_message_type` | `basic_id`, `location`, `authentication`, `self_id`, `system`, or `operator_id` |
| `rid_basic_id` | Exact, case-sensitive Basic ID text, 1–20 printable ASCII characters; serial (type 1) and CAA registration (type 2) only |
| `rid_basic_id_type` | String `"1"` (serial), `"2"` (CAA registration), `"3"` (UTM UUID), or `"4"` (specific session ID) |
| `rid_position_valid` | String `"true"`: Location message has in-range aircraft coordinates, with the unavailable `(0, 0)` pair excluded |

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

## Decoded Remote ID alert rules

The `rid_*` predicates work for both `wifi` and `ble` using the same bounded
[Remote ID decoder](../docs/remote-id.md) as telemetry logging. They require a
supported version-2 message in a valid envelope. A UUID or OUI association alone
does not satisfy them. `rid_valid` means structural decoding succeeded; it does
not assert that every optional field is available, validate authentication pages,
or prove aircraft identity or location. Basic ID text does not guarantee a vendor
or model name. Binary UTM/session IDs can be selected by type but are not treated
as text. Text matches require the complete value, including valid zero padding;
there is no implicit prefix matching.

For an `all` rule, **all RID predicates must match the same individual decoded
message**. Ordinary predicates still apply to the observation. Two Basic IDs in
one pack cannot supply each other's ID type/value. An ID predicate combined with
a Location/position predicate cannot match, even when the pack contains both:
there is deliberately no cross-message or cross-observation identity correlation.
Messages from earlier sightings are never consulted. For an `any` rule each
predicate is an independent alternative. In particular, `any` with a UUID and
`rid_valid` still allows UUID-only traffic; use `all` to require decoded evidence.
Aircraft position never uses the operator or receiver coordinates.

`examples/remote-id.json` supplies inactive, reviewed synthetic rules for any
valid RID and serial ID `WI-KNOW-TEST-001`, each for BLE and Wi-Fi. Copy it into
this directory, enable the desired entries, and enable the `drones` category.
The specific ID rule has higher priority than the general rule. All matches
produce classification records; only the highest-priority match controls buzzer
and follower actions. Existing observation timing/freshness and follower selection
remain applicable. Repeated sightings continue to match without bypassing the
**shared `buzzer.cooldown_ms`** rate limit (10 seconds in the supplied settings).
Enable and wire the buzzer before testing; keep a nonzero cooldown such as 10000.
The same cooldown is shared with following alerts, not reset for each message or
rule in a pack. Rules do not introduce a second alert path or a separate cache.
Schema version 1 and existing predicates remain compatible; new fields are
additive and preserved in the generated provenance manifest.

Potential upstream sources: [Eye Spy](https://github.com/simeononsecurity/eye-spy),
[OUI Spy](https://github.com/colonelpanichacks/oui-spy-unified-blue), and
[OpenDroneID](https://github.com/opendroneid/opendroneid-core-c). Review licensing,
fix the exact revision, and retain attribution when importing. Enable Flock, Axon,
ALPR, Ring, or drone categories only with reviewed signatures appropriate to the
radio/parser. Category switches alone do not add signatures. Decoded drone telemetry and rule predicates are documented in [Remote ID](../docs/remote-id.md).
