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
3. Otherwise write `utc_ms: null`, `time_source: "unknown"`, and display dashes.

Both streams expire after `gps.max_fix_age_ms` without a valid sample. Invalid RMC
revokes the GPS source immediately; invalid TIM revokes the receiver clock. A bad
checksum does not refresh either source. Samples are independent: receiving TIM
cannot displace fresh GPS time. Software interpolation is bounded by the same
freshness window; it never substitutes indefinite holdover for a missing receiver.
Monotonic boot/capture timestamps remain available regardless of UTC availability.

The LCD labels the clock `GPS`, `RTC`, or `?`; position validity is independent.
Logs include the selected source. `gps_sync_age_ms` is null until the first active
RMC and does not reset on RTC updates. Clock correction records preserve the prior
source. `time_source` events record fallback and loss, including transitions to
unknown, even when no valid time sample arrives. Buffered observations use a bounded mapping from a currently available
sample; old captures outside that window have no UTC. Sort by boot and monotonic
time, because corrected UTC can move backward.

## Protocol evidence and limits

The [Air530Z manual, page 16](https://files.seeedstudio.com/wiki/Grove-GPS_Air_530/Air530Z_Positioning_Module_User_Manual.pdf)
assigns TIM (`PCAS60`) to the eighteenth `PCAS03` payload field. The
[manufacturer CASIC protocol specification V4.2.0.3, section 1.6.12](https://wiki.wirenboard.com/wiki/images/5/53/AT6558-protocol-en.pdf)
defines receiver date/time and the time/leap validity flags. Its support note
mentions 5T firmware V5302 and later; availability varies by receiver firmware.

`rtc` means valid receiver-reported time used without active RMC. The protocol does
not expose a hardware-clock-origin or battery-health bit, and does not guarantee
that every Air530Z firmware supplies valid retained time before a satellite fix.
Using this stream as retained-clock fallback is an inference from its receiver-time
semantics, not a claim of measured hardware behavior. Unsupported output, absent
backup power, invalid flags, or a silent module leave UTC unknown. The
`backup_battery_present` setting does not override runtime validity checks.

This interface is enabled only for `Grove GPS Air530Z`. The older Air530's
`PGKC279`/`PGKC280` interface lacks equivalent validity evidence and is not queried
or treated as a valid clock. Its active RMC reception still works.

## Wiring and hardware acceptance

The serial console logs the configured GPS model, UART pins, baud rate, and whether
TIM requests are enabled at startup. Every 30 seconds it reports cumulative RX/TX
bytes and the number of complete TIM sentences observed. When TIM has been seen,
it also prints the latest raw `$PCAS60` sentence (including checksum) and its age.
Capture these lines before the first fix and again after acquiring a fix:

- `TIM_request=disabled`: check the firmware model and TX pin configuration.
- Increasing `tx_bytes` with `TIM_count=0`: the ESP32 is transmitting requests,
  but has not received a complete TIM sentence; this does not prove receiver
  acceptance. Check the receiver firmware's support and serial connection.
- A recent TIM with zero validity flags: the receiver is reporting time as invalid.
- A recent TIM with both validity flags set but `source=unknown`: retain the exact
  raw line to investigate checksum or field validation.

These diagnostics observe raw input, including invalid messages; they do not
relax the clock's validity checks. No TIM sentence means there is no raw line to
print. An old retained diagnostic line does not imply that the clock is usable.

Select the Air530Z parent/standalone profile. Connect MCU TX to GPS RX: GPIO8 on
N32R8 or GPIO0 on XIAO (remove the former amber LED/resistor). Keep GPS TX connected
to MCU RX (GPIO10 or GPIO12 respectively). `tx_pin: -1` skips output configuration;
receiver time can only be used if the module already outputs valid TIM messages.

Hardware acceptance has **not been run**. On the actual module, record firmware
version and raw UART output, then verify:

- After a previous GPS synchronization, power-cycle with backup power retained and
  reception blocked: valid TIM should show RTC; no valid TIM must show dashes.
- Restore reception: active RMC takes priority and shows GPS.
- Block reception again: valid TIM supplies RTC; invalid/silent TIM removes UTC.
- Repeat without backup power and with the module disconnected: no invented UTC.
- Reconnect or restart the receiver while scanning: commands recover its output.

Host tests cover validity/checksum rejection, date boundaries, source transitions,
expiration, correction metadata, null serialization, and partial TX/retry behavior.
