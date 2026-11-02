# Air530 retained time: evidence and validation gap

Issue #57 requests startup UTC before satellite reception. This change implements
the conservative fallback, advancing UTC display, independent software holdover,
and clock correction records. It does **not** claim validated retained-time startup.

## Documented interface

The [Air530 user manual V1.7](https://files.seeedstudio.com/wiki/Grove-GPS_Air_530/Air530_GPS_User_Booklet.V1.7.pdf),
printed page 16, documents `PGKC278` to set RTC date/time and `PGKC279` to query it:
`$PGKC279*23\r\n`. The `PGKC280` example returns year, month, day, hour,
minute, second, referencing the setter's fields. Its prose incorrectly repeats
the description of command 244; this is a documentation inconsistency requiring
bench confirmation. The reply layout provides no RTC-valid, oscillator-stop,
battery-health, or last-satellite-sync-age field. Pages 4–5 describe VBACKUP retention;
they do not establish that retained time is trustworthy after battery failure.

## Trust boundary

A checksum proves transport integrity. A calendar range check rejects impossible
dates, but cannot prove that a plausible date survived backup-power loss. Two
advancing replies would prove that a clock runs, not that it knows UTC. The
`backup_battery_present` setting cannot supply missing runtime validity evidence.
No raw startup capture or connected Air530 is available for this change. Therefore
`PGKC280` replies and `V` RMC sentences deliberately cannot initialize UTC. This
is a validation limitation, **not** a claim that Air530 has no RTC read command.

The manual board profiles now provide GPS TX on the former amber LED pin.
Firmware sends no speculative
commands, performs no startup wait, and leaves UTC unknown if the module is absent.
Only checksum-valid `A` RMC with a valid date/time initializes the software clock;
position is validated separately. Retained RTC is not reported as a source or fresh
satellite synchronization. After GPS synchronization, UTC advances even when
position expires; software holdover does not survive a parent reboot.

## Required hardware evidence before enabling retained UTC

1. Record module firmware version, board/wiring, baud rate, battery voltage and raw
   timestamped UART bytes from power-on through the first valid satellite fix.
2. On a bench setup, connect a suitable level-compatible MCU TX to Air530 RX;
   preserve the existing Air530 TX to MCU RX. Verify exposed pins and active GPIO
   conflicts for that board. Current board profiles use GPIO8 (N32R8) or GPIO0 (XIAO) for GPS TX.
3. Send the query above using bounded nonblocking TX and capture complete raw
   replies and checksums. Confirm the actual `PGKC280` fields and UTC semantics.
4. Repeat after a known satellite synchronization and main-power cycle while
   blocking reception, then with backup power disconnected/depleted, and on cold
   initialization. Include malformed replies and an absent module.
5. Establish a documented, reproducible way to distinguish invalid/uninitialized
   retained time, including plausible defaults. A healthy-battery demonstration
   alone is insufficient. Record source age as unknown before the first GPS sync.
6. Only after resolving validity, implement bounded query retries, retained source logging, RTC-to-GPS correction tests and hardware
   acceptance results. Keep ordering keyed to boot and monotonic capture time.

Hardware result: **not run**. No startup traces or battery-health results are
fabricated in this PR. Retained-time initialization remains follow-up work.

## Air530Z hardware clarification

The user's module is Air530Z. Its
[manual](https://files.seeedstudio.com/wiki/Grove-GPS_Air_530/Air530Z_Positioning_Module_User_Manual.pdf)
documents an AT6558R chipset, 3.3 V UART and PCAS commands. The Air530 PGKC query
above must not be assumed to work on Air530Z. The Z manual includes configurable
UTC/TIM outputs, but trustworthy retained-time interpretation still requires
protocol investigation and hardware evidence. The new TX wiring prepares for that
work and adds no startup query or wait.
