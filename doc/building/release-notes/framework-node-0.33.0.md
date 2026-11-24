[English](./framework-node-0.33.0.md) | [한국어](./framework-node-0.33.0.ko.md)

# ZLink Node.js Framework 0.33.0 Release Notes

Framework 0.33.0 uses Node.js binding 1.26.0 and Core 1.26.0. (#4846)

## Compatibility Changes

- A Spot or Actor handler owner type must equal the concrete registered type. A mismatch is a startup configuration error for both explicit and scan registration. Through 0.32.x, Node allowed registering subtypes. Split a shared parent-type handler into per-type handlers and call the shared logic as an ordinary method. (#4548, #4617)
- A Spot can no longer call its own close inside its own lifecycle callbacks (Join, Leave and so on); the call ends with InvalidOperation at the call site. Register a short timer in the callback and call close from the timer handler without waiting on it. The Bingo samples (five languages) and the Node TicTacToe sample now do this. (#4432, #4709, #4801)
- The channel header moved from JSON to protobuf (proto3). Because the wire format changes, 0.33.0 nodes and 0.32.x or older nodes cannot share a mesh; a mixed hello is rejected (required capability framework-service-v13 → v14). Upgrade every node in the mesh together. (#4352, #4713)
- The server Framework license changes from zlink License 1.0 to Elastic License 2.0. Apache-2.0 for client connectors, engine adapters, http-client, samples and templates, and MPL-2.0 for Core and bindings, are unchanged. (#3928, #4697)

## Behavior Changes

- Core 1.26.0 makes `zlink_disconnect` end only connect registrations and `zlink_unbind` end only bind registrations. Disconnecting the previous descriptor endpoint of a replaced node no longer closes the listener and inbound connections of a new node bound to the same endpoint. (#4846, #4864)
- Status queries and observation of registered RouteMesh, ClientServer and automatic fanout now return a snapshot (`Starting`, IsReady=false) before and during runtime start. Previously each language ended with an error such as InvalidOperation. Readiness code that caught status exceptions during startup can be removed. (#4800, #4812)
- ClientServer admission converts an opaque binary server RID into a single discovery string. (#4852, #4856)
- An Actor Join to a closed or inactive target Spot is rejected with Unavailable in all four languages. An Actor Join whose resolver confirmed a Closing authority ends with NotFound even if the activation disappears in between. (#4510, #4577, #4825, #4831)
- Caller cancellation and deadline results are published after local admission is returned, and an accepted request reacquires admission at its FIFO position before running. (#4286, #4676)
- Public calls before runtime start end with InvalidOperation at the terminal. (#4205, #4250)
- A same-node bind also goes through the owner Actor's duplicate-bind decision, and the duplicate-bind callback runs on the Actor lifecycle lane and a Spot execution gate turn. (#4501, #4516, #4771, #4784)
- Instance Spot send and request failures record target_rid, reason and error detail in message flow. (#4291, #4712)
- A request with no owner server information yet waits for a discovery refresh within its remaining time. The Location authority scan narrows providers by request kind and aligns page limits and progress rules across the four languages. (#4160, #4307, #4260, #4720)
- An ID direct request addressed to an Actor recreated on the same owner is handled by the current Ready incarnation. (#4520, #4601)
- A MeshNode appears as serving only after the host has published readiness. (#4565, #4566)
- A placement capacity failure of a Missing Instance request is also reported as a dispatch error. (#4775, #4842)
- During relocation Find returns the current Ready authority and the Entry Spot dispatches only to published Actors. The relocation target holds pre-cutover work and handles Message Follow on a departed route through the previous owner, with publication held at target admission. A plain Actor send bypasses the enclosing Spot and is received by the owner's current membership. (#4512, #4535, #4554, #4563, #4520, #4769)
- Stream bind decides the route generation from the Location authority fence instead of a remote lookup. Stream connector auto-reconnect waits the jitter from the first attempt after a disconnect. (#4462, #4776, #4413, #4656)
- The authority target generation is passed to request admission and the receiving source generation is read only at ingress. Authority routes are no longer rebuilt by a full periodic Store scan. New Actor placement candidates come from the live descriptor list, the owner lease is checked before placement, and bind uses one deadline. (#4335, #4470, #4260, #4264, #4205, #4214, #4160, #4174)
- A remote Entry Spot join uses the target descriptor's lifecycle generation. The authority of discarded Actors and Spots is cleaned up, and expired-peer cleanup treats a disconnect NOT_FOUND as already terminated. (#4461, #4791, #4644, #4301, #4311)
- Adds ownership handling during a Location Store outage. The owner lease TTL is 30 seconds and the release wait follows three margins. It includes the outage retention option and episode observation, the retention gate with discard after T (discarded work ends with not_found, stopped work with shutting_down), resuming preserved objects after recovery, reclaiming the lease only after normal expiry during an outage, and handling a second outage as a new episode. A renew that finds the lease expired outside an outage ends executions. (#3877, #3907, #3933, #3983, #3997, #4028, #4040, #4210, #4235, #4578)
- Adds the etcd Location Store provider, its etcd call layer and a Store maintenance tool. (#4078, #4095, #4097)

## Defect Fixes

- Owner fences are kept on published Actors only (a regression fix for #4578). (#4598)
- A session unbind failure after destroy goes to the post-commit error owner. Each owned store and the execution context are disposed once across teardown retries. (#4387, #4584, #3923, #4051)
- Host shutdown records diagnostics at each await boundary, enabled in template CI. (#4627, #4710)

## Performance Improvements

- The first four channel header fields and the closing string are no longer rebuilt for every message. (#4352, #4872)
- Per-message provider lookup, authority reinterpretation, control frame creation and receive-wait abort are removed. (#4540, #4549)
- Default typed JSON byte fields convert with Node's native Base64. Unnecessary copies and revalidation in envelope header decoding are removed. (#4354, #4416, #4352, #4414)
- Drops one dispatch-worker await per claim and carries the mailbox receive time into general records. (#3872, #3894, #4064)

## Dependencies

- The Framework now depends on protobufjs 8.8.1 (for the protobuf channel header). (#4713)

## Install

```bash
npm install @zlink-systems/framework@0.33.0 @zlink-systems/http-client@0.33.0
```

The release tag is [`framework-node/v0.33.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.33.0).
