# Node ladder call shape — lfx-node

| Step | Binding path (file:line) | Ladder path (file:line) | Difference belongs to |
|---|---|---|---|
| Source send | `framework/bench/grpc/node/client/main.js:259-276,307-324` — protobuf/raw submit path | Same file/lines; L2 adds envelope, L3 service frame, L4 typed payload | L2/L3/L4 named feature |
| Target receive | `zlink-raw-server/main.js:47-53,64-75` — `DontWait`, NoData idle, capped pump | `zlink-framework-server/main.js:142-155` — same nonblocking receive loop and cap | Same receive; L1 permit excluded, L2-L4 codec/frame only |
| Target reply | `zlink-raw-server/main.js:77-86` — new protobuf response body, two parts | `zlink-framework-server/main.js:168-176,116-133` — same L0 handler/body; L2 envelope; L3 frame; L4 typed codec | L2 header, L3 frame, L4 codec |
| Source completion / reply receive | `client/main.js:273-298` — common completion pump, protobuf decode | Same lines; L2 header parse, L3 unframe, L4 typed decode | L2/L3/L4 named feature |

L2 comparison enables `ZLINK_LADDER_L0_SERVER=1`, `ZLINK_LADDER_L0_CLIENT=1`, `ZLINK_LADDER_L2_ENVELOPE=1`, `ZLINK_LADDER_L2_CLIENT=1`. L3 and L4 use the corresponding switches from the copied ladder runner. L0R/L2R/L3R are disabled.

## Per-message costs

| Path | Waits | Allocations | Atomics | Copies / encoding |
|---|---|---|---|---|
| Binding raw / L0 | One `DontWait` receive attempt per message; batch max 256; `setImmediate` per nonempty pump; source completion polling yields with `setImmediate` | Decoded protobuf payload and newly encoded reply payload; receive objects reused | 0 JS atomics | Static raw envelope reused; protobuf creates body buffer; no explicit `Buffer.from` body copy |
| L1 | Above plus `applicationJobQueue.acquire()` before receive and permit release before handler | Permit plus suspended promise/continuation when needed | 0 JS atomics | Same as L0 |
| L2 | No extra async wait | Request/reply header parsed once per side; response header encoded per reply | 0 JS atomics | Adds envelope header buffer; body remains protobuf |
| L3 | No extra async wait; same completion pump | Service correlation, mapped arrays, frame and decoded wrappers per request/reply | 0 JS atomics | Packs multipart bytes into service frame; unpack views frame and creates owned parts |
| L4 | No extra async wait; same completion pump | Typed payload/header and Message wrappers on request/reply | 0 JS atomics | JSON/base64 typed codec; `Buffer.from` on decode/body conversion and UTF-8 encode |

Sources: `framework/bench/grpc/node/zlink-raw-server/main.js:47-86`, `zlink-framework-server/main.js:119-180,232-280`, `shared/raw-wire.js:94-155,167-250`, `client/main.js:259-335`.
