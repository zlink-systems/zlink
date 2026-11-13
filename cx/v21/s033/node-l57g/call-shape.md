# Node L5~L7 call shape for diagnosis

| Step | Binding call (file:line) | L-step call (file:line) | Same? / restored feature |
|---|---|---|---|
| Source send | `client/main.js:259-276,307-324` raw request/send; `zlink-raw-server/main.js:61-86` server raw protocol | L5~L7 selects `client/framework-transport.js:67-73`; server registers handlers in `zlink-framework-server/main.js:355-359` | Route and packet contract intended to match; Framework transport + operation lifecycle is the restored feature |
| Target receive | `zlink-raw-server/main.js:47-53,64-75` direct `DontWait` receive and pump | Production dispatch parses packet in `framework/languages/node/packages/framework/src/runtime/channels/channel-dispatchers.ts:170-201`; dispatch stages are `channel-dispatch-pipeline.ts:107-149` | Different by restored L5~L7 Framework route dispatch |
| Target reply | `zlink-raw-server/main.js:77-86` new protobuf reply body and submit | `zlink-framework-server/main.js:355-359` registers request handler; Framework writes reply after handler completion | Different by restored L5~L7 Framework handler/completion path |
| Source completion / reply receive | `client/main.js:273-298` source polls completion and decodes reply | L5~L7 request uses public `client.requestToNode(...).submit()` at `client/framework-transport.js:51-62` | Different by restored L5~L7 Framework operation/completion path |

## Per-message work

| Path | Waits | Allocations | Atomics | Copies / encoding |
|---|---|---|---|---|
| Binding raw / L0 | Target: one `DontWait` receive attempt per message, batch capped at 256, then one `setImmediate` per nonempty pump. Request source completion polls completion and yields with `setImmediate` until reply settles. | One decoded protobuf payload and one encoded reply payload per request; `Received` objects reused per socket. | 0 JavaScript atomics in the bench loop. | Reuse static raw envelope; protobuf serializer creates body buffer; no explicit `Buffer.from` body copy in raw path. L0 does the same wire work and calls its fixed handler directly. |
| L1 | Adds one `applicationJobQueue.acquire()` before receive and releases permit before handler work; may suspend while waiting for a permit. | One queue permit per received message plus a promise/continuation when acquisition suspends. | 0 JavaScript atomics in the bench loop. | Same envelope/body path as L0; no additional explicit body copy. |
| L2 | No additional asynchronous wait. | One request/reply header value parsed per side and a response header encoded per reply. | 0 JavaScript atomics in the bench loop. | Adds an envelope header buffer; protobuf body encode/decode remains raw. |
| L3 | No additional asynchronous wait; request completion still uses raw completion pump. | Per request: correlation value, mapped parts array, frame, decoded wrappers; replies similarly allocate a frame and wrappers. | 0 JavaScript atomics in the bench loop. | Packs multipart bytes into service frame; unpack uses frame view and `fromOwned` parts. |
| L4 | No additional asynchronous wait; request completion still uses raw completion pump. | Typed payload/header values and `Message` wrappers on request and reply. | 0 JavaScript atomics in the bench loop. | Typed JSON/base64 path copies body bytes with `Buffer.from` on decode and JSON text to bytes on encode. |
| L5~L7 | Public Framework client operation submit/completion; target dispatches through owner queue, worker activation, handler scope, registry and completion/deadline handling. | Framework-owned operation/queue/worker/scope/completion objects; exact count is hidden inside production runtime. | Native runtime synchronization is Framework-owned; no extra JS atomics in bench path. | Framework typed serializer and transport; no extra explicit bench `Buffer.from` copy in `framework-transport.js`. |

Sources: `/root/jobs/fx/cx/v21/s033/lad-node-s1/call-shape.md` and current worktree at `7d1afa98a9f53c952daf0fd3437691b0c5b123ce`.
