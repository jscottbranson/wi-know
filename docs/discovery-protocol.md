# Universal discovery and runtime identity

The universal image uses the factory base MAC as its six-byte discovery UUID.
The short RS-485 address is a separate, volatile assignment. A new random 32-bit
boot session distinguishes restarts; assignments also carry a parent-issued
monotonically advancing token. These identifiers prevent accidental stale traffic
from affecting another assignment; this local-bus protocol is not authenticated.

## Framing and messages

CRC-protected, little-endian framing is version 5. All nodes must be updated
together; earlier framing versions are rejected. Reports retain the Wi-Fi subtype
and bounded MAC header and distinguish BLE advertising from connected primary-service
discovery. Discovery envelopes and message types retain their existing layout.
Manual poll/report types 1/2 remain supported only by manually configured builds.
Universal builds use types 3–8:

| Type | Purpose |
| --- | --- |
| 3, discover | Parent announces its MAC/session, sweep nonce and response slot |
| 4, hello | Matching unassigned worker reports factory MAC, boot and capabilities |
| 5, assign | Parent offers address, child/follower role and session token |
| 6, assigned | Worker acknowledges that exact assignment |
| 7, session_poll | Identity envelope followed by the existing scan command |
| 8, session_report | Identity envelope followed by the existing report |

Every new message starts with a 32-byte envelope: discovery version (currently
1), role, capability bits, slot, parent MAC (6 bytes), worker MAC (6 bytes), then
four little-endian uint32 values: parent boot, worker boot, assignment token,
and discovery nonce. Capability bits are Wi-Fi 2.4 GHz, Wi-Fi 5 GHz and following.
Following includes Wi-Fi target tracking even if BLE is disabled. Identity-only
messages must be exactly 32 bytes. Unknown framing/envelope versions, invalid
roles/slots, CRC errors and malformed payloads are rejected. Decoder errors are
reported in serial logs.

## Response opportunities and collisions

There are 32 discovery slots per sweep. The parent explicitly queries **one**
slot, waits at most `bus.poll_timeout_ms` for its response, and interleaves normal
worker polls. An unassigned worker responds only if the published hash of its
MAC, boot session and the sweep's fresh random nonce selects that slot. Assigned
workers stay silent during discovery. Each worker transmits at most once per
sweep; no autonomous retries or bursts can seize the bus.

A collided/corrupt hello receives no assignment. A new random nonce at the next
sweep changes the opportunities; workers retry then. This gives bounded airtime
and backoff per attempt, not a hard upper bound on admission under arbitrary
noise. Discovery continues for hot-plug boards and capacity recovery. Serial CRC
errors and assignment/capacity failures expose trouble; workers log their MAC,
state, last address and role every ten seconds. A healthy default sweep normally
takes several seconds and can take longer while assignments retry.

Each discovery exchange includes an actual random 1–64 ms receive interval
before transmission, in addition to a randomized 100–150 ms scheduling interval.
The receive interval remains in force even when other bus operations run late.
It prevents two simultaneous parents from remaining locked to identical periodic
transmissions. Startup also listens for 2–4 seconds before transmitting. Any
foreign discover, assignment, session poll, or legacy manual poll latches a parent
conflict: that parent stops all bus transmission until reboot. A worker that sees
a second parent while leased also halts until reboot. The parent display shows
`PARENT CONFLICT / Bus stopped`; serial logs explain the stop. Remove or rewire
the unintended parent and reboot conflicted boards. Do not connect mixed manual
and universal clusters. Electrical arbitration and collision recovery must still
be verified on the physical RS-485 network as described in the acceptance test.

## Assignments, roles and recovery

The parent assigns the first free address in the configured inclusive range
(1–16 maximum). Capabilities are checked before admission. Repeated hello messages
for the same MAC/boot return the existing offer; a different boot using a leased
MAC is deferred until its prior lease expires. Capacity exhaustion does not evict
an existing assignment and never creates a duplicate address.

With one discovered worker, it scans. With two or more, `follower.enabled=true`
reserves one eligible worker as the dedicated follower, leaving the others to
share scanning work through the existing channel planner. The highest assigned
eligible address is chosen initially; an existing follower stays selected as
more boards join. After its lease expires another eligible worker replaces it.
If only the follower survives, it becomes a scanning child. Disabling
`follower.enabled` makes every worker scan. Per-MAC role/address overrides are
not provided; the NVS override described in the wiring document controls only
parent versus worker identification.

Every role change issues a new token and requires another assignment ACK before
normal polling. The parent refreshes the current discovery nonce in repeated
offers, so losing an assignment or ACK does not wedge retries across sweeps.
A worker accepts an offer only for its factory MAC/current boot and the last
parent probe it heard. Older tokens remain rejected after lease expiry; an expired token cannot be
reactivated. A repeated live offer is acknowledged idempotently without renewing
the lease or resetting command sequence/capture state.
The parent accepts an ACK/report only for the matching MACs, boots, address,
role and token. Commands must additionally have a newer sequence number. A
role-only reassignment retains observation delivery acknowledgements; reusing
an address for another MAC resets them.

Identity leases use `bus.assignment_lease_ms`; accepted session polls/reports
renew them on both ends. Capture stops at either scan-command or identity lease
expiry. `bus.membership_lease_ms` is shorter: missing workers first lose scan
coverage and channel-plan membership, then lose their reserved identities.
No address, role or lease is persisted. A parent reboot starts a fresh session;
workers wait out their old lease, rediscover and receive new addresses. A worker
reboot similarly waits for the parent's old reservation to expire. Reconnecting
within a valid lease can resume polling. Replacement hardware has a new MAC and
receives a free address. MAC/address mappings remain stable within a live lease,
not across complete cluster restarts. Factory MACs remain the durable identities
in assignment logs.

## Scheduling and capture continuity

The generator validates leases against two conservative complete node sweeps.
Universal mode budgets three request/response exchanges per address (discovery,
assignment ACK, normal poll), request serialization, the random receive interval
and task scheduling slack. Configurations that cannot maintain the leases are
rejected, including heavily populated slow buses. Ordinary extra observation
polls retain the bounded burst budget and always allow discovery/peripheral work.

A parent with no live scanning children time-slices local Wi-Fi channels and BLE
while continuing discovery. When a scanning child reports online, local Wi-Fi
stops and parent BLE continues; scanning resumes locally when all scanning
children disappear from membership. A follower alone is not counted as scanning
coverage. When the follower is reclassified as a child, normal cluster scanning
can resume. This policy prioritizes keeping discovery and the parent responsive;
local scans are sequential, and their currently tuned channel is displayed
separately rather than falsely claiming full simultaneous coverage.

Worker coverage counts only successful tuning reports from the current role,
boot, command and membership. Offline or reassigned workers lose coverage
immediately. Local radio errors remain visible. Admission policy and node address
are captured together at reception, before deferred parsing. Queue contents and
retained observations survive assignment, role and local/cluster transitions;
only new capture admission changes. Already admitted data is delivered under the
next valid session. RAM data cannot survive a worker reboot, and a parent reboot
may permit duplicate delivery because receipt history is volatile.

Host tests cover framing/version rejection, hardware selection/override rules,
stale session and token rejection, lease expiry, collision retries, capacity,
duplicate MAC boots, address reuse, role thresholds/replacement, and removal of
stale membership. Physical boot ordering, bus collisions, harness voltages and
RF performance are not claimed as validated by host tests or CI builds.
