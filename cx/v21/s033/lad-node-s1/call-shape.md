# Node ladder call shape — before measurement

| Step | Binding call (file:line) | L-step call (file:line) | Same? / restored feature |
|---|---|---|---|
| Source send | `client/main.js:259-276,307-324` — raw envelope/body; request submit or send submit | L0–L4: `client/main.js:259-276,307-324` — L2 envelope, L3 service frame, L4 typed payload. L5~L7: `client/main.js:352-360` selects `framework-transport.js:67-73` (`sendToNode`) | Yes by intended route; L2 envelope, L3 framing, L4 codec, L5~L7 production transport/lifecycle are the restored features |
| Target receive | `zlink-raw-server/main.js:47-53,64-75` — `DontWait`, NoData becomes `false`, batch pump | `zlink-framework-server/main.js:142-155` — `DontWait`, NoData breaks, same batch bound | Yes: both treat NoData as idle and use same nonblocking loop. L1 adds permit at `main.js:232-260`; L5~L7 switches to production runtime path |
| Target reply | `zlink-raw-server/main.js:77-86` — decode, encode a new protobuf body, submit two parts | `zlink-framework-server/main.js:168-176,116-133` — fixed handler and new body at L0; L2 adds reply envelope; L3 adds service frame; L4 typed reply; L5~L7 production completion path | L0 same wire operations; additional work maps to L2 envelope, L3 framing, L4 codec, L5~L7 operation lifecycle |
| Source completion / reply receive | `client/main.js:273-298` — await reply completion and decode protobuf | `client/main.js:273-298` — same common raw operation; L2 parses reply header, L3 unpacks service wire, L4 typed decode | Yes at L0; L2 header parse, L3 unframing, L4 typed codec are restored features; L5~L7 remains server-side |

All call-shape paths are from the current worktree at `7d1afa98a9`. L0/L1 use `ZLINK_LADDER_L0_SERVER=1` and `ZLINK_LADDER_L0_CLIENT=1`; later step switches add only the named feature. L0R/L2R/L3R are excluded (module-load controls).

## L5~L7 send-saturation observation

The L5~L7 warm run used the Framework source path above and the server registered `addSendHandler` (`zlink-framework-server/main.js:355-359`), but the raw result has `submitted=completed=75055` and `target_stats.received=0` (`raw/node/L5_7/send-saturation-1024/warm/attempt1/.../results.json`). This warm run is invalid and excluded; it does not establish a valid L5~L7 send-saturation cell. The source/target route and receive count need further measurement evidence before that cell can be reported.
