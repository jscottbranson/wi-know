# Optional GPS-aware following scoring

Issue #38 extends the existing `FollowingDetector`; it does not add a second
following alarm. All shipped profiles keep `following_alert.scoring_enabled`
false. Existing `enabled: true` installations retain their earlier distance/delay
behavior until scoring is explicitly enabled. GPS must also be enabled.

## Evidence and identity

The parent (including standalone mode) scores capture-time observations using the
existing GPS history and worker clock mapping. Every encounter belongs to one
`(radio, address_type, address)` tuple, regardless of channel or receiver node.
There is no class-level identity correlation. A rotating BLE address starts a new
encounter: this deliberately misses some rotating trackers instead of merging
unrelated people's trackers into one following device. An address is an observed
identifier, not proof of a physical device or an owner's identity.

The generic feature also considers unknown addresses. It does not load disabled
rule categories or classify trackers. Disabling `ble_trackers` prevents those
signature matches, but does not exempt unknown BLE addresses from this separately
enabled generic feature. Use `ignore_devices` for known devices. Shipped category
choices are unchanged.

## Configuration

These fields are in every example profile's `following_alert` section. Older
configuration files may omit all the new fields; the generator supplies defaults.
For a GPS-equipped deployment, set `enabled` and `scoring_enabled` true:

| Setting | Default | Meaning |
|---|---:|---|
| `distance_m` | 200 | Minimum radial displacement from the encounter's first receiver fix |
| `delay_ms` | 30000 | Continuous time outside the displacement radius before alerting |
| `max_gap_ms` | 15000 | Longest gap between sightings before a new encounter |
| `min_duration_ms` | 300000 | Minimum encounter duration; 1 second–24 hours |
| `min_hits` | 3 | Minimum counted sightings; 2–1,000,000 |
| `hit_interval_ms` | 1000 | Minimum interval between counted hits; 100 ms–1 hour, no longer than `max_gap_ms` when enabled |
| `score_threshold` | 3 | Required score and saturation limit; 2–1,000,000 |
| `score_decay_ms` | 60000 | One point decays per complete interval; 1 second–24 hours |
| `gps_jitter_m` | 20 | Assumed per-fix jitter allowance; 1–1000 m |
| `max_speed_mps` | 100 | Maximum plausible receiver speed; 1–500 m/s |
| `max_fix_age_ms` | 2000 | Maximum distance in time from capture to fix; 0–60000 ms |

Example tuning for a standalone sweep longer than 15 seconds: raise `max_gap_ms`
above the complete Wi-Fi/BLE revisit interval (for example 30000), keep
`hit_interval_ms` at 1000, and ensure sightings recur faster than score decay.
The same settings work for parent/worker scanning. No profile silently relaxes
GPS or duration requirements. Unrealistically fast decay can intentionally make
the threshold unreachable; choose intervals based on observed scan cadence.

The first sighting contributes one hit/point. Each later eligible hit adds one
point up to the threshold. Complete decay intervals are applied before the new
point, carrying fractional time forward; score never goes below zero. Duration,
hits and current score must all qualify. Duplicate and out-of-order capture times
do not count or extend continuity. Distinct receiver reports with different
capture times are rate-limited by `hit_interval_ms`.

Stationary qualifying encounters emit `device_persistence` once and show
“Device persists” without a buzzer. Following additionally requires displacement
at least `distance_m + 2 * gps_jitter_m`, sustained for `delay_ms`; then it emits
one `following_alert`, shows “Following alert”, and requests the existing shared
buzzer cooldown. Returning inside the radius resets the outside timer. Repeated
circles and GPS jitter do not accumulate path length. The jitter setting is an
assumption, not measured GPS accuracy or a guaranteed error bound.

## Loss, bounds and logging

Invalid/nonfinite coordinates, stale GPS, and impossible jumps reset that
address's encounter. A jump exceeds `2 * gps_jitter_m + max_speed_mps * fix_time_delta`;
a backward fix timestamp also resets. After GPS loss or a rejected jump, a fresh
fix starts a new origin. Fixes slightly after the capture may be selected by the
existing nearest-fix GPS history. Both global GPS age/timing uncertainty checks
and the scoring age limit apply. No GPS means no following claim and no scored
persistence. Captures older than `max_gap_ms` at arrival (or from the future)
cannot trigger alerts. An arrival-clock rollback clears all encounters; older
capture timestamps alone are treated as delayed reports. Reboot clears all state.

The fixed table holds 64 encounters, evicting the least recently observed when
full, with no allocations. Expiry is checked lazily on a later sighting and old
slots are eligible for eviction. Queue and wire formats are unchanged: workers
send the existing observation with capture metadata; scoring stays at the parent.

JSONL records include `following_score`, `following_reset`, `device_persistence`
and `following_alert`, with reason, hits, current score, duration, displacement,
jitter allowance, identity basis, capture timestamp, timing uncertainty and
receiver fix. Counted hits/decay and transitions are logged; uncounted rapid hits
and duplicates are silent. The initial record includes the new-encounter/gap/
capacity-replacement/clock-reset reason. Reset records preserve pre-reset counts.
A capacity replacement does not emit a separate record for the evicted address.
Records follow the existing matched/unmatched retention class and exporter
(JSON/JSONL/CSV/TSV/GeoJSON); a matches-only storage policy can omit generic
unmatched encounter evidence. They do not claim coordinates of the remote device.

## Upstream review and intentional differences

Reviewed Eye Spy revision `5dba436175fd670a67eb819c2e17407d6fe918e8`,
[`src/es_detect.h`](https://github.com/simeononsecurity/eye-spy/blob/5dba436175fd670a67eb819c2e17407d6fe918e8/src/es_detect.h)
and [`src/main.cpp`](https://github.com/simeononsecurity/eye-spy/blob/5dba436175fd670a67eb819c2e17407d6fe918e8/src/main.cpp).
Its tracker gate uses class-wide continuity (30 minutes, three hits, five-minute
gap); unknown-device persistence uses per-address hits and duration. This original
implementation adapts the duration/hit/continuity concepts, adds GPS movement
requirements and per-identity decay, and deliberately does not adopt class-wide
correlation or global threat-score escalation. This is not behavioral parity.
Eye Spy attribution, Apache-2.0 license and upstream NOTICE remain in
`rules/sources/` and the root `NOTICE`.

Also reviewed OUI Spy Unified Blue revision
`46eb13b2746ae72ad7070bbf5e42deb09b573623`, `src/raw/detector.cpp`: its persisted
history/aliases are distinct from this GPS encounter score; no implementation or
history inference was imported. Its MIT notice remains in `rules/sources/`.

## Validation and remaining hardware acceptance

Host tests cover stationary persistence, movement, one alert per encounter,
returning inside radius, jitter, jumps, GPS loss/reacquisition, stale/invalid fixes,
late/future/duplicate/out-of-order reports, radio/address-type separation,
rotation, gaps, decay, clock rollback, ignore entries, disabled behavior, bounded
eviction, generated settings and retained/exported evidence. Existing legacy
following tests remain unchanged.

Controlled stationary and moving runs on C5 with real GPS remain required for
issue #38: check GPS errors, parent/worker time alignment, standalone revisit
cadence, delayed queues, UI/buzzer cooldown and heap/task headroom under Wi-Fi,
BLE and storage load. No physical following-detection performance is claimed by
host tests or successful firmware builds.
