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
