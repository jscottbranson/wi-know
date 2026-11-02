# Air530Z receiver RTC fallback

Air530Z parent/standalone firmware requests receiver time at boot without waiting
for a fix. It enables RMC and TIM with a volatile `PCAS03` command. TX uses only
available UART FIFO space; startup retries and a 30-second refresh recover delayed
startup or a later receiver reset. No commands save configuration to module flash,
set the date, or reset the receiver. Disabling unused output avoids serial backlog
at 9600 baud.

## Time selection

1. Use fresh checksum-valid active RMC date/time as `gps`.
2. Otherwise use fresh `PCAS60` receiver time as `rtc`, only if `timeValid=1`,
   `leapsValid=1`, and the UTC calendar/time fields are valid (2000–2099).
3. Otherwise use fresh checksum-valid **void RMC (`V`) date/time** as approximate
   `rtc` time on Air530Z. Calendar/time fields must parse as 2000–2099. The receiver
   does not certify this estimate as valid; its accuracy and RTC origin are unknown.
   This fallback never supplies position or counts as a GPS synchronization.
4. Otherwise write `utc_ms: null`, `time_source: "unknown"`, and display dashes.

All three samples expire after `gps.max_fix_age_ms` without an accepted update.
Void RMC revokes GPS time immediately and can update the approximate fallback.
Missing/malformed RMC date/time or an unrecognized status revokes that fallback;
active RMC replaces it with GPS time. Invalid TIM revokes only the TIM sample,
leaving a fresh RMC estimate available. A bad checksum refreshes no source. Samples are independent: receiving TIM
cannot displace fresh GPS time. Software interpolation is bounded by the same
freshness window; it never substitutes indefinite holdover for a missing receiver.
Monotonic boot/capture timestamps remain available regardless of UTC availability.

The LCD labels the clock `GPS`, `RTC`, or `?`; position validity is independent.
Logs include the selected source. `gps_sync_age_ms` is null until the first active
RMC and does not reset on RTC updates. Buffered observations use a bounded mapping
from a currently available sample; old captures outside that window have no UTC.
Sort by boot and monotonic time, because corrected UTC can move backward.

## Protocol evidence and limits

The [Air530Z manual, page 16](https://files.seeedstudio.com/wiki/Grove-GPS_Air_530/Air530Z_Positioning_Module_User_Manual.pdf)
assigns TIM (`PCAS60`) to the eighteenth `PCAS03` payload field. The
[manufacturer CASIC protocol specification V4.2.0.3, section 1.6.12](https://wiki.wirenboard.com/wiki/images/5/53/AT6558-protocol-en.pdf)
defines receiver date/time and the time/leap validity flags. Its support note
mentions 5T firmware V5302 and later; availability varies by receiver firmware.

**Older Air530Z firmware may not support the `PCAS60` RTC/time interface.**
On a user's Air530Z reporting `URANUS5,V5.3.0.0`, repeated firmware queries
returned clean TXT replies but no TIM sentences. The module did send plausible
pre-fix time in `$GNRMC,221032.000,V,,,,,,,021126,,,M,V*2C`. This is consistent
with firmware predating the documented TIM support, rather than proof of a
failed RTC or backup battery. The approximate RMC fallback accommodates it.

`rtc` now covers both validated TIM and approximate void-RMC time. Neither the
label nor `backup_battery_present` proves hardware-clock origin, battery health,
or accuracy. A plausible but incorrect receiver date can therefore appear in
logs; `utc_uncertainty_ms` remains null rather than claiming measured accuracy.
An active GPS sample takes priority and can correct the estimate in either
direction. Missing TIM alone no longer forces unknown time when usable void RMC
is arriving. No usable date/time or a silent module still leaves UTC unknown.

The CASIC specification includes a firmware-upgrade mechanism, but this repository
provides no GPS firmware image or upgrade procedure. Obtain a module-specific image
and instructions from Seeed/Hezhou before attempting an update; updating the ESP32
application does not update the GPS module.

This interface is enabled only for `Grove GPS Air530Z`. The older Air530's
`PGKC279`/`PGKC280` interface lacks equivalent validity evidence and is not queried
or treated as a valid clock. Its active RMC reception still works.

## Wiring and hardware acceptance

Select the Air530Z parent/standalone profile. Connect MCU TX to GPS RX: GPIO8 on
N32R8 or GPIO0 on XIAO (remove the former amber LED/resistor). Keep GPS TX connected
to MCU RX (GPIO10 or GPIO12 respectively). `tx_pin: -1` skips output configuration;
receiver time can only be used if the module already outputs valid TIM messages.

The full hardware acceptance checklist has **not been run**; the pre-fix
V5.3.0.0 sample above is a user-reported observation. On the actual module, record firmware
version and raw UART output, then verify:

- After a previous GPS synchronization, power-cycle with backup power retained and
  reception blocked: valid TIM or usable void-RMC time should show RTC; neither must show dashes.
- Restore reception: active RMC takes priority and shows GPS.
- Block reception again: valid TIM or usable void RMC supplies RTC; silence expires UTC.
- Repeat without backup power: record whether void RMC supplies an incorrect estimate.
- Disconnect the module: UTC must expire rather than continue indefinitely.
- Reconnect or restart the receiver while scanning: commands recover its output.

Host tests cover validity/checksum rejection, date boundaries, source transitions,
expiration, null serialization, and partial TX/retry behavior.
