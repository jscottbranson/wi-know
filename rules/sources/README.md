# Signature sources and attribution

The repository's CC0 dedication does not replace third-party notices or grant
rights to third-party source databases.

## OUI Spy (MIT)

Five rules in `../surveillance.json` adapt identifier facts from
[colonelpanichacks/oui-spy-unified-blue](https://github.com/colonelpanichacks/oui-spy-unified-blue/tree/46eb13b2746ae72ad7070bbf5e42deb09b573623),
revision `46eb13b2746ae72ad7070bbf5e42deb09b573623`:

- `src/raw/axeoff.cpp`: Axon/TASER company ID and 16-bit service UUIDs.
- `src/raw/flockyou_promiscious.cpp`: Flock provisioning SSID, Penguin/external
  battery names, and XUNTONG company ID used only with a Penguin name.

Copyright (c) 2026 colonelpanichacks. The complete upstream MIT notice is in
[oui-spy-LICENSE.txt](oui-spy-LICENSE.txt); retain it when redistributing these
adaptations. No upstream detection implementation was copied. The Flock source
describes these identifiers as derived from a camera firmware dump dated
2026-09-16. No firmware image, firmware code, or dump is included here; the
upstream MIT notice is not a license grant for that proprietary firmware.

## IEEE registry facts

Twenty rules use vendor assignments checked against the
[IEEE public MA-L CSV](https://standards-oui.ieee.org/oui/oui.csv), downloaded
2026-10-28. Full-download SHA-256:

`10ac72b5d9178cab63ab4f49458b946e9d1997902340229eeb5443929d738c3a`

[ieee-surveillance-2026-10-28.csv](ieee-surveillance-2026-10-28.csv) retains only
the 21 selected assignment/vendor rows, without organization addresses. The
full database is not bundled. IEEE publishes the registry for download, but an
explicit redistribution license for the database has not been established in
this review. Neither the extract nor the registry is represented as MIT or CC0;
the rule provenance records this limitation. Attribution alone is not a claim
that every potential database right has been resolved.

## Research consulted, not imported

[ReconGrunt/FlipDeFlock](https://github.com/ReconGrunt/FlipDeFlock/tree/343f87e5dc38a350e5a1ad20be9e18545ff1f14e)
(GPLv3 license text at that revision) was consulted for false-positive pitfalls,
vendor candidates, and parser limitations. No code or tables were copied from
that project. The vendor assignments used here were obtained directly from
IEEE. This attribution records research provenance; it does not relicense
FlipDeFlock material under CC0 or MIT.

## Expanded pack: modified adaptations (2026-10-28)

`../expanded.json` is a modified adaptation, not an upstream source file. Each
rule carries its exact source URL, revision, license and review date. See the
root [NOTICE](../../NOTICE) for retained copyright and modification notices.

- **Eye Spy**, SimeonOnSecurity, revision
  `5dba436175fd670a67eb819c2e17407d6fe918e8`: `src/es_detect.h` pattern tables
  and `src/main.cpp` tracker/MeshCore/iBeacon/Remote ID identifiers. Apache-2.0;
  [license](eye-spy-LICENSE.txt) and unmodified [NOTICE](eye-spy-NOTICE.txt).
  Camera/vendor tables are attributed to Eye Spy, not presented as a fresh IEEE
  registry verification. Upstream high-confidence/shared-chipset labels are not
  inherited: broad vendor observations are explicitly possible associations.
- **OUI Spy Unified Blue**, colonelpanichacks, pinned revision already listed
  above: `src/raw/detector.cpp` Meta composite and names. MIT notice retained.
- **Flock You**, colonelpanichacks, revision
  `6704f011f3e6c9cfe94fe18c593f863032c9c954`: `main.cpp` ordered probe signature.
  [MIT notice](flock-you-LICENSE.txt). Upstream credits NitekryDPaul and
  DeFlockJoplin for this community drive-test fingerprint. The adaptation requires
  strict valid probe IEs, a wildcard SSID and a vendor OUI, does not salvage corrupt
  frames, and retains `possible` confidence. No upstream parser code is copied.
- Decoded Remote ID alert rules are locally authored using the existing decoder.

Eye Spy and Flock You cite GainSec, wgreenberg, dougborg, NitekryDPaul and
DeFlockJoplin for underlying research. Firmware-derived identifiers are cited
through those open-source projects; their licenses do not license proprietary
firmware. No firmware image or code is copied. FlipDeFlock remains research-only,
with no GPL source or signature table imported.

The original CC0 dedication does not relicense Apache/MIT adaptations. Retain
NOTICE, these upstream notices/licenses and the generated provenance manifest
when distributing source or firmware. CI universal firmware artifacts include
these files. The full IEEE database is still not bundled.
