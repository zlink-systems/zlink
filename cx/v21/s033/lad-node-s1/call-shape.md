# Node ladder call shape — before measurement

| Step | Binding call (file:line) | L-step call (file:line) | Same? / restored feature |
|---|---|---|---|
| Source send | `client/main.js:259-276,307-324` — raw envelope/body; request submit or send submit | `client/main.js:259-276,307-324` — L2 selects production envelope; L3 adds service frame; L4 selects typed payload | Yes at L0; L2 envelope, L3 framing, L4 typed codec are the restored step features |
| Target receive | `zlink-raw-server/main.js:47-53,64-75` — `DontWait`, NoData becomes `false`, batch pump | `zlink-framework-server/main.js:142-155` — `DontWait`, NoData breaks, same batch bound | Yes: both treat NoData as idle and use same nonblocking loop. L1 adds permit at `main.js:232-260`; L5~L7 switches to production runtime path |
| Target reply | `zlink-raw-server/main.js:77-86` — decode, encode a new protobuf body, submit two parts | `zlink-framework-server/main.js:168-176,116-133` — fixed handler and new body at L0; L2 adds reply envelope; L3 adds service frame; L4 typed reply; L5~L7 production completion path | L0 same wire operations; additional work maps to L2 envelope, L3 framing, L4 codec, L5~L7 operation lifecycle |
| Source completion / reply receive | `client/main.js:273-298` — await reply completion and decode protobuf | `client/main.js:273-298` — same common raw operation; L2 parses reply header, L3 unpacks service wire, L4 typed decode | Yes at L0; L2 header parse, L3 unframing, L4 typed codec are restored features; L5~L7 remains server-side |

All call-shape paths are from the current worktree at `7d1afa98a9`. L0/L1 use `ZLINK_LADDER_L0_SERVER=1` and `ZLINK_LADDER_L0_CLIENT=1`; later step switches add only the named feature. L0R/L2R/L3R are excluded (module-load controls).
