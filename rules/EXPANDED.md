# Expanded detection pack

Reviewed 2026-10-28. `expanded.json` adds 67 rules to the existing 25-rule
surveillance pack: 92 total, all enabled in every shipped profile. These are
passive observations, not authenticated identities. No physical-device
validation has been performed. Attribution, modifications and license obligations
are recorded in [NOTICE](../NOTICE) and [source notes](sources/README.md).

| Category | Evidence and interpretation | Default |
| --- | --- | --- |
| `cameras` | Eye Spy camera-vendor Wi-Fi/BLE OUIs and advertised SSID keywords, including Axis, Hikvision, Dahua, Reolink, Wyze, Arlo, Nest, Amcrest, Vivotek, Hanwha, FLIR and Mobotix candidates | On |
| `unifi_cameras` | Ubiquiti vendor OUIs on Wi-Fi/BLE; `unifi`/`protect` advertised SSID keywords. Also matches APs and other equipment; not camera-specific | On |
| `ble_trackers` | Tile FEED/FEEC, Samsung FD5A, Apple Find My manufacturer prefixes, iBeacon prefix | On |
| `flock` | Additional case-insensitive SSID/BLE names, two exact firmware-default MACs, accessory UUID, advertised Raven 3100–3500 services, seven vendor-qualified community probe patterns | On |
| `shared_radio_vendors` | Community Flock candidate OUIs, LiteOn/USI/Qualcomm and Motorola vendor evidence; these identify shared radio hardware, not Flock or ALPR | On |
| `acoustic_sensors` | SoundThinking/ShotSpotter vendor OUI on Wi-Fi/BLE | On |
| `smart_glasses` | Luxottica company ID AND Meta service in one advertisement; Ray-Ban/Wayfarer/Oakley Meta name alternatives | On |
| `serial_modules` | Exact HC-03/05/06 names; generic serial modules also used by skimmers, not proof of a skimmer | On |
| `device_services` | Nordic DFU UUID or DfuTarg name; generic firmware-update service, not proof of Flock | On |
| `mesh_radios` | MeshCore advertised name prefix | On |
| `drones` | Advertised Remote ID service (possible); structurally decoded Remote ID on Wi-Fi/BLE (broadcast) | On |
| `alpr` | Additional advertised network keywords; existing Genetec rules remain vendor-only evidence | On |

Existing Axon, body-camera, Flock, Ring and Genetec coverage remains enabled.
All single indicators, including the community probe pattern, are `possible`.
The Meta composite is `probable`. Neither means authenticated identity.
Broad keywords such as `cam`, `ring`, `axis`, `lpr` and `protect` can occur in
unrelated names; shared OUIs can be very noisy. Disable their categories if needed.
A UniFi-branded device may independently match a generic camera keyword; disabling
`unifi_cameras` disables that category, not every possible classification of that device.

Every config enables all shipped detection categories and leaves
`detection.disabled_categories` empty. To opt out, move a category from
`detection.enabled_categories` to `detection.disabled_categories`, then
rebuild/reflash. A category cannot be in both lists. Omitted
`disabled_categories` defaults to `[]` for existing configs. Rules stay individually
enabled in the source pack; the generated manifest records their effective state.
The limit is now
128 rules, leaving 36 slots for custom rules. The inactive lab templates remain
outside the production rule directory glob.

Tracker sightings do not establish that a tracker is following someone. Apple
Find My advertisements can come from other Find My products, and iBeacon is a
generic beacon format. No class-wide continuity inference or rotating-address
identity claim is imported. The independent existing following-alert setting is
unchanged. HC modules using Bluetooth Classic only cannot be seen by the C5 BLE
receiver. Only services actually advertised are observed; no GATT queries occur.

## Additional predicate formats

- `ssid_contains_ci`, `name_contains_ci`: nonempty printable UTF-8, at most 32
  encoded bytes. ASCII case-insensitive substring matching of captured display
  text (up to 32 bytes, stopping at NUL). SSIDs exclude probe requests.
- `name_exact`: case-sensitive equality with captured BLE name text.
- `service_uuid16_range`: inclusive `3100-3500` hex range. Checks 16-bit service
  lists/data and equivalent Bluetooth-base 128-bit lists/data. Other 128-bit UUIDs
  do not qualify. Service data uses only its leading UUID. Partial UUIDs invalidate
  the whole match. An equal-bound range selects one service in either encoding.
- `wifi_ie_signature`: comma-separated ordered decimal IE tags, skipping SSID;
  vendor tags use `221:` plus the first min(length, 8) body bytes in hex. Other
  bodies are ignored. This is distinct from exact `wifi_ie_fingerprint` bytes.
  The community pattern is combined with a wildcard SSID and one LiteOn/USI OUI.
  Valid strict probe IEs are required; no malformed-frame salvage, phantom skips,
  prefix matching, or reconstruction of missing tags is performed. Raw complete
  IE evidence remains in logs. Shared stacks can still match.

## Coverage decisions

The pack adapts all Eye Spy signature families that the existing passive Wi-Fi
and BLE observation model can represent, plus OUI Spy's safer Meta composite
and Flock You's ordered IE pattern. Weak shared-chipset and DFU evidence has
its own category. The local-address `82:6b:f2` prefix is omitted: an OUI lookup
must not assert a vendor for locally administered addresses. Bare ten-digit BLE
names are omitted as nonspecific. Receiver-address/data-frame matching, Bluetooth
Classic, active GATT discovery, and upstream class-wide persistence scoring are
not implemented. BWCDEVICE from GPL-licensed FlipDeFlock remains research-only;
no GPL detector implementation or table is incorporated. No proprietary firmware
or firmware dump is distributed. See source notes for exact pinned revisions.
