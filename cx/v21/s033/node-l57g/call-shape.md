# Node L5~L7 call shape for diagnosis

| Step | Binding call (file:line) | L-step call (file:line) | Same? / restored feature |
|---|---|---|---|
| Source send | `client/main.js:259-276,307-324` raw request/send; `zlink-raw-server/main.js:61-86` server raw protocol | L5~L7 selects `client/framework-transport.js:67-73` and server registers handlers in `zlink-framework-server/main.js:355-359` | Source/target route and packet contract intended to match; Framework transport + operation lifecycle is restored feature |
| Target receive | `zlink-raw-server/main.js:47-53,64-75` direct `DontWait` receive and pump | `framework/languages/node/packages/framework/src/runtime/channels/channel-dispatchers.ts` dispatches production route messages (trace fields are built at `channel-dispatch-pipeline.ts:117-140`) | Different by restored L5~L7 Framework route dispatch |
| Target reply | `zlink-raw-server/main.js:77-86` new protobuf reply body and submit | `zlink-framework-server/main.js:355-359` registers request handler; Framework writes the reply after handler completion | Different by restored L5~L7 Framework handler/completion path |
| Source completion / reply receive | `client/main.js:273-298` source polls completion and decodes reply | L5~L7 request uses public `client.requestToNode(...).submit()` at `client/framework-transport.js:51-53` | Different by restored L5~L7 Framework operation/completion path |

Per-message receive/reply costs are the same comparison already enumerated in `lad-node-s1/call-shape.md` § Per-message costs. These controls are diagnostic smokes, not performance cells; no throughput verdict is drawn from them.
