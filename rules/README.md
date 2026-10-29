# Detection matching rules

Place production rule files directly in this directory as `*.json`. Every such
file is validated and compiled into firmware. Subdirectories are excluded, so
`examples/` contains inactive templates. Rebuild/reflash the parent after edits.
`surveillance.json` supplies the local surveillance rule pack described below;
`rules.json` remains available for additional local entries.
An empty enabled ruleset produces a visible build/startup diagnostic.

See [expanded detection coverage and configuration](EXPANDED.md) for the additional
67 rules, default opt-outs, matcher formats, and limitations.

## Local surveillance pack (reviewed 2026-10-28)

`surveillance.json` contains 25 enabled rules. The local settings and all shipped
example profiles enable its categories, including `body_cameras` for Utility,
Digital Ally, and legacy VIEVU. Existing custom profiles must add `body_cameras`
to `detection.enabled_categories` to enable those rules.
The separate [expanded pack](EXPANDED.md) includes Ubiquiti/UniFi identifiers, enabled by default alongside BLE trackers in every profile.

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

The Wi-Fi parser accepts beacons, probe responses, bounded probe requests, and
supported NAN Remote ID actions, not ordinary client data. Probe requests require
independently reviewed rules; adding parser support does not identify probe-only
Flock devices. Requested SSIDs never match `ssid_prefix` rules. BLE OUI rules only
work with public, globally administered addresses. Names/company IDs can still match random
addresses. Raw `BWCDEVICE` payload substring matching is unsupported and was
not approximated. Native 128-bit service UUID matching is supported. Exact probe IE fingerprints are
supported as described below; the expanded pack also adds a separate normalized community probe signature. No signal
does not mean no camera.

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
| `ssid_prefix` | Case-sensitive UTF-8 advertised Wi-Fi SSID prefix, at most 32 encoded bytes; excludes probe requests |
| `wifi_subtype` | `probe_request`, `probe_response`, `beacon`, or `nan_action` |
| `wifi_ssid_wildcard` | `"true"` for a present empty probe SSID, `"false"` for a present nonempty probe SSID |
| `wifi_ie_fingerprint` | Full ordered probe IE bytes as contiguous hex, at most 255 bytes; canonical lowercase |
| `wifi_requested_ssid_prefix` | Case-sensitive printable UTF-8 prefix of a probe request's raw SSID bytes, at most 32 encoded bytes |
| `name_prefix` | Case-sensitive UTF-8 BLE advertised name prefix, at most 32 encoded bytes |
| `manufacturer_prefix` | Hex prefix of BLE AD type `0xff`, including little-endian company ID bytes |
| `service_uuid16` | Conventional 16-bit UUID text, e.g. `180F`; compared against little-endian AD bytes |
| `service_uuid128` | BLE canonical UUID text, e.g. `00112233-4455-6677-8899-aabbccddeeff`; case-insensitive input, normalized to lowercase |
| `rid_valid` | String `"true"`: at least one structurally valid supported decoded Remote ID message |
| `rid_message_type` | `basic_id`, `location`, `authentication`, `self_id`, `system`, or `operator_id` |
| `rid_basic_id` | Exact, case-sensitive Basic ID text, 1–20 printable ASCII characters; serial (type 1) and CAA registration (type 2) only |
| `rid_basic_id_type` | String `"1"` (serial), `"2"` (CAA registration), `"3"` (UTM UUID), or `"4"` (specific session ID) |
| `rid_position_valid` | String `"true"`: Location message has in-range aircraft coordinates, with the unavailable `(0, 0)` pair excluded |

Wi-Fi beacons/probe responses, bounded probe requests, supported passive NAN
Remote ID action frames, and complete BLE advertising reports are parsed. Retained
payload evidence is limited to 255 bytes; oversized payloads are rejected without
truncation. The display name/SSID and `ssid_prefix`/`name_prefix` retain up to 32
received bytes, stopping at the first NUL byte. `wifi_requested_ssid_prefix` instead
uses the raw SSID IE length and bytes, including embedded NULs. Prefixes are exact
and case-sensitive, including UTF-8; rule text cannot contain ASCII control
characters or exceed 32 encoded bytes. Regex is not supported. JSONL retains
SSID/name display text, `ssid_hex` for that display prefix, BLE advertising hex,
and complete `wifi_ie_hex` for probe requests with an explicit evidence format.
Every matching rule produces a classification record with its category,
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
field of protocol v3 reports and the JSONL `address` field; exact MAC matching
adds no wire or log fields. The separate probe-evidence feature introduced v3. Local/standalone and forwarded observations use the same
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

## Passive probe requests and exact IE evidence

`wifi_subtype` accepts `probe_request`, `probe_response`, `beacon`, or `nan_action`.
`wifi_ssid_wildcard` accepts `true` (one zero-length SSID IE) or `false` (one
nonempty SSID IE), on probe requests only. A missing SSID matches neither value.
Duplicate SSIDs are rejected. `ssid_prefix` continues matching advertised network
names in beacons/probe responses, and deliberately does not match requested probe
SSIDs: requesting a network does not establish the requesting device's vendor.

`wifi_requested_ssid_prefix` matches a case-sensitive, nonempty prefix of the
raw SSID IE in a probe request. Values are printable UTF-8, at most 32 bytes. It
never matches beacon/probe-response SSIDs, wildcard probes, or missing SSIDs.
The raw retained IE bytes are authoritative, including embedded NUL bytes; the
display `ssid` text does not control this predicate. Public and locally administered
transmitter addresses are both supported.

This can surface clients requesting a network name such as `Flock-…`, using a
separate rule category like `requested_network`, confidence `possible`, and a
clear ID such as `requested-flock-like-network`:

```json
"predicates": [
  {"field": "wifi_subtype", "value": "probe_request"},
  {"field": "wifi_requested_ssid_prefix", "value": "Flock-"}
]
```

Use `match: "all"`; make buzzer/follow actions an explicit local choice. The
example prefix already appears in the reviewed advertised-network rules, but
this request evidence has a different meaning. It means only that a client
requested that name, potentially from saved-network behavior. It does not prove
a connection attempt, a saved network, current/past association, a phone (rather
than another kind of client), the client's manufacturer, ownership, or affiliation.
Network names can be generic/spoofed; private client addresses rotate. No generic
Axon or Motorola network prefix is assumed: those require separately reviewed
name evidence. This capability adds no enabled production requested-network rules.

`wifi_ie_fingerprint` accepts 2–510 contiguous hexadecimal characters, case
insensitive and canonicalized to lowercase in generated code and provenance.
Its format is **probe_ie_v1**: the complete, ordered IE stream after the 24-byte
management header, including every element ID, length, and value byte. Repeated
non-SSID IEs remain repeated; order matters. Vendor IEs include their complete OUI,
type and remaining bytes (at least four body bytes are required); extension IEs
include the extension ID. No field is normalized: directed SSID bytes (including
embedded NUL), rates, channel declarations, vendor sequence fields and all other
variable data participate. Capture timestamp, RSSI, address, sequence-control and
FCS are outside this stream. This deliberately conservative exact comparison has
no short-hash collision or prefix-match ambiguity; variation means a non-match.
Unknown well-formed IEs are retained. Malformed/truncated IEs, duplicate/oversize
SSIDs, incomplete vendor/extension identifiers and fingerprints over 255 bytes
are rejected. Empty IE streams can be observed but cannot be fingerprint rules.

Use `match: "all"` to combine four predicates: `wifi_subtype: "probe_request"`,
`wifi_ssid_wildcard: "true"`, an independently reviewed address/identifier predicate,
and `wifi_ie_fingerprint`. The third predicate can be `mac`; an OUI provides only vendor association and does not prove device identity.
A wildcard probe or shared chipset fingerprint alone must never assert Flock
identity. Shared defaults, random/private addresses, spoofing and common radio
stacks remain limitations. The original exact-byte predicate added no production signatures. The [expanded pack](EXPANDED.md) now supplies a separate normalized probe pattern and shared-vendor rules.
The test fixtures are newly authored synthetic bytes under this repository's
license. Any future external fingerprint requires a pinned source, license review,
and independent confidence/evidence review before enabling it.

See [probe evidence transport and hardware checks](../docs/probe-capture.md).

### 128-bit BLE service UUIDs

`service_uuid128` accepts exactly 8-4-4-4-12 hexadecimal digits with hyphens.
Braces, URN prefixes, missing/misplaced separators, masks and wildcards are rejected.
Generated C++ and the provenance manifest contain lowercase canonical text.
BLE sends the entire 16-byte UUID least-significant octet first: the example
above matches `ffeeddccbbaa99887766554433221100` in advertising data.

Matching checks complete entries in incomplete/complete service lists (AD types
`0x06`/`0x07`) and only the leading UUID in 128-bit service data (`0x21`). It never
searches application data or manufacturer data for UUID bytes. A partial UUID in
any of these elements makes this predicate fail for the whole report, even if
another element matches; list lengths must be multiples of 16, and service data
must have at least 16 UUID bytes. An empty service list contains no match. A zero
AD length terminates parsing, consistently with other BLE predicates.

Public and random advertiser addresses use the same UUID comparison. This is
passive advertising matching, with no GATT connection or service discovery.
The existing 255-byte complete-report/reassembly limit, scan-boundary quarantine,
observation identity and deduplication policies are unchanged. Raw AD evidence
already crosses the child-parent bus and is retained as `ble_ad_hex` in
JSONL; no wire or log version change is required. Standalone and parent matching
therefore receive identical UUID bytes. Use `all` to combine independent evidence,
or `any` for alternatives, as with `service_uuid16`.

A service UUID is a broadcast claim, not authentication or proof of a particular
manufacturer. Generic Nordic DFU services must not be labeled Flock-specific.
The [expanded pack](EXPANDED.md) supplies reviewed service signatures. UniFi and
Ubiquiti have a separate category disabled in all shipped profiles.

Hardware validation for issue #32 remains **not performed**. Before closing it,
use a controlled advertiser and actual XIAO ESP32-C5 receivers to:

1. Advertise a synthetic UUID above in `0x06`, `0x07`, and leading `0x21` service
   data, separately, using public and random addresses. Verify passive delivery
   and matching with a temporary lab rule and no active discovery.
2. Repeat with complete legacy reports and bounded extended advertising, including
   split UUID bytes; verify over-255-byte/truncated reports cannot match and scan
   restarts do not join fragments across boundaries.
3. Compare standalone and child-parent captures, classification/actions, raw
   `ble_ad_hex`, and generated `.rules.json` provenance. Test a UUID appearing
   only inside service-data bytes and one-byte near misses; neither should match.
4. Record firmware commit, advertiser setup, controller report behavior, JSONL,
   and scan/queue counters. Host tests are software evidence, not hardware proof.
