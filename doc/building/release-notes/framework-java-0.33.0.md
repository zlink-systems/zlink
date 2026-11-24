[English](./framework-java-0.33.0.md) | [한국어](./framework-java-0.33.0.ko.md)

# ZLink Java and Kotlin Framework 0.33.0 Release Notes

Framework 0.33.0 uses Java and Kotlin binding 1.26.0 and Core 1.26.0. (#4846)

## Compatibility Changes

- A Spot or Actor handler owner type must equal the concrete registered type. A mismatch is a startup configuration error for both explicit and scan registration. Through 0.32.x, the Java handler catalog allowed registering subtypes. Split a shared parent-type handler into per-type handlers and call the shared logic as an ordinary method. (#4548, #4617)
- A Spot can no longer call its own close inside its own lifecycle callbacks (Join, Leave and so on); the call ends with InvalidOperation at the call site. Register a short timer in the callback and call close from the timer handler without waiting on it. The Bingo samples (five languages) and the Node TicTacToe sample now do this. (#4432, #4709, #4801)
- Java typed JSON treats a reference field without a visible `@Nullable` (JSpecify) as non-null. Sending or receiving null fails serialization or deserialization, so annotate nullable fields with `@Nullable` (use a `?` type in Kotlin). Framework core now delivers JSpecify at runtime. (#4412, #4515, #4798)
- Execution controls without a public contract moved behind an internal bridge, and the Kotlin API snapshot is checked in CI. The three public API additions are recorded in the JVM API snapshot. (#4747, #4767, #4105, #4178)
- The channel header moved from JSON to protobuf (proto3). Because the wire format changes, 0.33.0 nodes and 0.32.x or older nodes cannot share a mesh; a mixed hello is rejected (required capability framework-service-v13 → v14). Upgrade every node in the mesh together. (#4352, #4713)
- The server Framework license changes from zlink License 1.0 to Elastic License 2.0. Apache-2.0 for client connectors, engine adapters, http-client, samples and templates, and MPL-2.0 for Core and bindings, are unchanged. (#3928, #4697)

## Behavior Changes

- Core 1.26.0 makes `zlink_disconnect` end only connect registrations and `zlink_unbind` end only bind registrations. Disconnecting the previous descriptor endpoint of a replaced node no longer closes the listener and inbound connections of a new node bound to the same endpoint. (#4846, #4864)
- Status queries and observation of registered RouteMesh, ClientServer and automatic fanout now return a snapshot (`Starting`, IsReady=false) before and during runtime start. Previously each language ended with an error such as InvalidOperation. Readiness code that caught status exceptions during startup can be removed. (#4800, #4812)
- An Actor Join to a closed or inactive target Spot is rejected with Unavailable in all four languages. An Actor Join whose resolver confirmed a Closing authority ends with NotFound even if the activation disappears in between. (#4510, #4577, #4825, #4831)
- Caller cancellation and deadline results are published after local admission is returned, and an accepted request reacquires admission at its FIFO position before running. (#4286, #4676)
- Public calls end at one host admission point: InvalidOperation before start and ShuttingDown after stop. A lease admission rejection is classified as ShuttingDown. (#4205, #4266, #3877, #4199)
- Actor request and send publish the caller result after local admission cleanup, and a deadline is published after dispatch cancellation and cleanup. (#3877, #4281)
- A same-node bind also goes through the owner Actor's duplicate-bind decision. (#4501, #4516)
- Runtime weight setters only record the value and mark the publisher; descriptor publication is left to the renew schedule. When Serving, one publish is requested immediately, and the mark stays until publication is confirmed. (#4858, #4861, #4893)
- Instance Spot send and request failures record target_rid, reason and error detail in message flow. (#4291, #4712)
- A request with no owner server information yet waits for a discovery refresh within its remaining time. The Location authority scan narrows providers by request kind and aligns page limits and progress rules across the four languages. (#4160, #4307, #4260, #4720)
- An ID direct request addressed to an Actor recreated on the same owner is handled by the current Ready incarnation. (#4520, #4601)
- A remote Entry Spot join uses the target descriptor's lifecycle generation. The owner generation decided by the authority is passed to request admission, and the authority decision comes first. The Actor request deadline includes location lookup time. (#4461, #4791, #4335, #4407, #4543, #4592)
- A session bind keeps one entry deadline. Authority routes are no longer rebuilt by a full periodic Store scan, and the scan prefix is passed down to the Store. Expired-peer cleanup treats a disconnect NOT_FOUND as already terminated. (#4160, #4182, #4260, #4268, #4301, #4311)
- The authority of discarded Actors and Spots is cleaned up, and a renew that finds the lease stale outside an outage discards executions. HELLO and ADMIT share the first admission snapshot of a route so a peer does not stay in CONNECTING. (#4644, #4625, #4518, #4728)
- Adds ownership handling during a Location Store outage. The owner lease TTL is 30 seconds and the release wait follows three margins. It includes the outage retention option and episode observation, the retention gate with discard after T, resuming preserved objects after recovery, reclaiming the lease only after normal expiry during an outage, refreshing Session fence copies after an owner lease change, and handling a second outage as a new episode. (#3877, #3906, #3935, #3970, #3990, #4008, #4022, #4033, #4044, #4211, #4238)
- Adds the etcd Location Store provider. (#4080, #4106)

## Defect Fixes

- A request accepted by the target queue runs with its original payload even after the caller times out or cancels. Previously the payload was closed on caller-side termination, so the accepted work entered the handler with an empty payload. (#4825, #4827)
- STREAM recv no longer waits for send on the socket state lane. Stream sessions are looked up by routing id bytes and the public sessionId is created once. (#4568, #4748, #4572)
- An Instance message addressed to a later incarnation of the same owner goes to the owner queue instead of a cold activation. (#3505, #4483)
- The string form of a framework error now includes the kind and the submit cause. (#4489, #4495)
- The testkit fake mesh send builds headers in the public channel header format. (#4352, #4844)
- Nullable fields in samples now carry `@Nullable` (ShoppingMall and TicTacToe messages, morpg-fields-instances PartyCommandResult). (#4412, #4802, #4839)

## Performance Improvements

- Typed JSON duplicate-property checking uses a per-object bitmask, cutting 1K decode B/op from 10,688 to 6,024. (#4354, #4830)
- Typed JSON bytes decode without Base64 round trips. (#4354, #4419)
- Callers wait for queued source results instead of being handed the lane. (#4355, #4788)
- The envelope header is decoded in one parser pass using registration-prepared names and a canonical header reader, with unnecessary copies and revalidation removed. The correlation id is built from one hex conversion of a 16-byte array. (#3870, #3893, #3952, #4352, #4414, #4445)
- Request dispatch is prepared at bind time with binding frames kept, and the final typed receiver is bound before submission. (#3871, #3981, #3989)

## Dependencies

- The Java Framework core is shaded, so the POM does not change. JSpecify is delivered at runtime by the typed JSON profile package. (#4713, #4798)

## Install

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.33.0")
    implementation("systems.zlink:zlink-http-client:0.33.0")
}
```

The release tag is [`framework-java/v0.33.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.33.0).
