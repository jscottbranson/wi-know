# Detection matching rules

Place production rule files directly in this directory as `*.json`. Every such
file is validated and compiled into firmware. Subdirectories are excluded, so
`examples/` contains inactive templates. Rebuild/reflash the parent after edits.
`surveillance.json` supplies the local surveillance rule pack described below;
`rules.json` remains available for additional local entries.
An empty enabled ruleset produces a visible build/startup diagnostic.

## Local surveillance pack (reviewed 2026-10-28)

`surveillance.json` contains 25 enabled rules. The local settings and all shipped
example profiles enable its categories, including `body_cameras` for Utility,
Digital Ally, and legacy VIEVU. Existing custom profiles must add `body_cameras`
to `detection.enabled_categories` to enable those rules.
No UniFi/Ubiquiti Wi-Fi or Bluetooth identifiers are included.

| Category | Coverage | Interpretation |
| --- | --- | --- |
| `axon` | Registered OUI on Wi-Fi/BLE; BLE company ID `034D` and services `FC81`, `FE6B`, `FE6C` | Possible Axon/TASER equipment, including body cameras, Fleet, and other equipment |
| `body_cameras` | Utility, Digital Ally, legacy VIEVU registered OUIs on Wi-Fi/BLE | Possible vendor equipment; may include in-car cameras or accessories |
| `flock` | Registered `B4:1E:52` OUI, provisioning SSID prefix, Penguin/external battery BLE names | Possible Flock equipment or accessory; does not distinguish ALPR, other cameras, and batteries |
| `ring` | All 13 Ring LLC MA-L registrations in the reviewed IEEE snapshot, on Wi-Fi/BLE | Possible Ring device, including cameras, doorbells, chimes, and other products |
| `alpr` | Two Genetec registered OUIs on Wi-Fi/BLE | Possible Genetec vendor association only; not proof of an AutoVu plate reader or that it has a wireless radio |

OUI groups use `any`, with at most four prefixes per rule. All single-indicator
rules are `possible`; only a `Penguin-` name AND XUNTONG company ID `09C8` in
the same BLE observation is `probable` (a battery/accessory association).
Manufacturer prefixes include the company ID in little-endian order: `4D03`
for Axon/TASER and `C809` for XUNTONG. Names are case-sensitive; only listed
variants match. Prefix rules cannot validate a serial suffix, so a name such as
`Flock-Guest` also matches and must remain only possible.

Rules request buzzer and follower actions, subject to existing global settings.
OUI matches have priority 30, Flock names 40, and the composite battery rule 60.
The supplied local buzzer is disabled. Matches indicate observed identifiers,
not authenticated identity, camera recording state, or a person following you.

### Sources and deliberate omissions

See [source attribution and licensing notes](sources/README.md) for the imported
MIT notice, IEEE database licensing limitation, and research-only references.

Registered vendor addresses were checked directly against the
[IEEE MA-L registry](https://standards-oui.ieee.org/oui/oui.csv).
The selected factual rows are retained in
`sources/ieee-surveillance-2026-10-28.csv`; each rule records the full downloaded
registry's SHA-256. This is a factual extract, not a claim that IEEE licenses its
whole database under this project's license.

Axon BLE and Flock naming facts are adapted from
[OUI Spy at revision 46eb13b](https://github.com/colonelpanichacks/oui-spy-unified-blue/tree/46eb13b2746ae72ad7070bbf5e42deb09b573623/src/raw).
Each rule links its exact source file; its MIT notice is preserved in
`sources/oui-spy-LICENSE.txt`. These community signatures have not been tested
against physical devices here.

Generic Espressif, Qualcomm, LiteOn, Amazon, Motorola, and other shared hardware
prefixes are omitted. Neither XUNTONG company ID alone nor Nordic DFU services
identify Flock. No unsupported Vigilant, ELSAG, Rekor, or generic ALPR fingerprint
is asserted. In particular, Genetec vendor matching does not provide universal
ALPR detection.

The current Wi-Fi parser accepts beacons/probe responses, not ordinary client
data or probe requests. A client-mode Ring camera or probe-only Flock camera can
therefore remain invisible to these rules. BLE OUI rules only work with public,
globally administered addresses. Names/company IDs can still match random
addresses. Raw `BWCDEVICE` payload substring matching, 128-bit service UUIDs,
and Wi-Fi information-element fingerprints are unsupported and were not
approximated. No signal does not mean no camera.

Validate with `python3 tools/generate_config.py`, then rebuild/reflash the parent
or universal firmware using the existing firmware workflow. Merely editing JSON
does not update a running device.

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
| `mac` | Exact six-byte transmitter/advertiser address, e.g. `02:11:22:33:44:55`; supports local/random addresses |
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

Wi-Fi beacons/probe responses, supported passive NAN Remote ID action frames,
and complete BLE advertising reports are parsed within the 255-byte observation
payload limit. Oversized payloads are rejected without truncation. Text matching retains up to 32 received bytes of
the name/SSID, stopping at the first NUL byte. Prefixes match those bytes exactly,
including UTF-8; they cannot contain ASCII control characters or exceed the
32-byte limit. Regex is not supported. Match evidence is retained as SSID/name,
`ssid_hex` for that captured text prefix, and BLE advertising hex in
JSONL. Every matching rule produces a classification record with its category,
confidence and priority; records for the same capture share the same observation
identity and count. The highest-priority match controls alerts and target selection.
OUI matches indicate a possible vendor association, not confirmed equipment identity.

`mac` accepts exactly six colon-separated, two-digit hexadecimal octets, with
uppercase or lowercase digits. The generator normalizes the value to lowercase
in both C++ and the provenance manifest. Compact, hyphenated, space-separated,
masked, wildcard, and partial addresses are rejected. Matching compares all six
bytes of the Wi-Fi transmitter (address 2) or BLE advertiser; it does not inspect
Wi-Fi receiver or BSSID addresses. The predicate is not address-type qualified:
identical BLE bytes match on both public and random addresses, and locally
administered Wi-Fi addresses match too. Existing `oui` exclusions are unchanged.
Private addresses rotate and cannot establish persistent identity. A firmware-default
MAC is neither vendor authentication nor proof of a particular device: defaults
can be shared and addresses spoofed. Production signatures still require separate
source and confidence review. No new vendor signatures are included by this capability.

Exact address evidence already travels in the six-byte `Observation.address`
field of protocol v2 reports and the JSONL `address` field; no wire or log format
expansion is needed. Local/standalone and forwarded observations use the same
matcher, priority selection, and action flags.

Every build also writes `settings.rules.json` beside its generated `settings.hpp`.
This manifest preserves all compiled rules, their predicates, confidence, actions,
source URL/revision/license/review date, and enabled state after category filtering.
Share it alongside observation logs to explain the signatures behind each rule ID.
Keep the manifest from the same build as the observations; a later rebuild can
change a rule while retaining its ID.

## Decoded Remote ID alert rules

The `rid_*` predicates work for both `wifi` (beacon and supported NAN) and `ble` using the same bounded
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
