[English](./framework-dotnet-0.33.0.md) | [한국어](./framework-dotnet-0.33.0.ko.md)

# ZLink .NET Framework 0.33.0 Release Notes

Framework 0.33.0 uses .NET binding 1.26.0 and Core 1.26.0. (#4846)

## Compatibility Changes

- A Spot or Actor handler owner type must equal the concrete registered type. A mismatch is a startup configuration error for both explicit and scan registration. Through 0.32.x, .NET scan registration silently skipped mismatched handlers. Split a shared parent-type handler into per-type handlers and call the shared logic as an ordinary method. (#4548, #4617)
- A Spot can no longer call its own close inside its own lifecycle callbacks (Join, Leave and so on); the call ends with InvalidOperation at the call site. Register a short timer in the callback and call close from the timer handler without waiting on it. The Bingo samples (five languages) and the Node TicTacToe sample now do this. (#4432, #4709, #4801)
- The channel header moved from JSON to protobuf (proto3). Because the wire format changes, 0.33.0 nodes and 0.32.x or older nodes cannot share a mesh; a mixed hello is rejected (required capability framework-service-v13 → v14). Upgrade every node in the mesh together. (#4352, #4713)
- The server Framework license changes from zlink License 1.0 to Elastic License 2.0. Apache-2.0 for client connectors, engine adapters, http-client, samples and templates, and MPL-2.0 for Core and bindings, are unchanged. (#3928, #4697)

## Behavior Changes

- Core 1.26.0 makes `zlink_disconnect` end only connect registrations and `zlink_unbind` end only bind registrations. Disconnecting the previous descriptor endpoint of a replaced node no longer closes the listener and inbound connections of a new node bound to the same endpoint. (#4846, #4864)
- Status queries and observation of registered RouteMesh, ClientServer and automatic fanout now return a snapshot (`Starting`, IsReady=false) before and during runtime start. Previously each language ended with an error such as InvalidOperation. Readiness code that caught status exceptions during startup can be removed. (#4800, #4812)
- A handler turn an Actor already started is resumed and completed even after the Actor is destroyed; only new payloads are rejected. The reason for rejecting a resume is distinguished by kind and message (lost execution right is NotFound, seal or Spot shutdown is ShuttingDown). (#4800, #4806)
- An Actor Join to a closed or inactive target Spot is rejected with Unavailable in all four languages. An Actor Join whose resolver confirmed a Closing authority ends with NotFound even if the activation disappears in between. (#4510, #4577, #4825, #4831)
- Caller cancellation and deadline results are published after local admission is returned, and an accepted request reacquires admission at its FIFO position before running. (#4286, #4676)
- Public calls end with InvalidOperation before the runtime starts and ShuttingDown after it stops. A send or publish call is marked as used only after admission accepts it. (#4205, #4251, #4295)
- The typed JSON input contract is enforced and pinned with a fixture shared by the four languages. (#4412, #4515)
- Instance Spot send and request failures record target_rid, reason and error detail in message flow. (#4291, #4712)
- A request with no owner server information yet waits for a discovery refresh within its remaining time. The Location authority scan narrows providers by request kind and aligns page limits and progress rules across the four languages. (#4160, #4307, #4260, #4720)
- An ID direct request addressed to an Actor recreated on the same owner is handled by the current Ready incarnation. (#4520, #4601)
- A Spot direct message is accepted by the current Ready incarnation, and authority observation invalidates the route cache. During Close, an Instance message for a new Ready incarnation on the same owner is received by the owner activation instead of going to Reserve. (#4506, #4528, #3505, #4488)
- The authority decision comes before the route decision, and a request waits on a peer of a different generation. Actor create and Instance Spot wait for the authority generation. The Actor request deadline includes location lookup time. (#4335, #4401, #4428, #4543, #4592)
- A session bind keeps one entry deadline. The authority of discarded Actors and Spots is cleaned up. HELLO and ADMIT share the first admission snapshot of a route so a peer does not stay in CONNECTING. (#4160, #4182, #4644, #4518, #4728)
- The duplicate-bind callback runs on the Actor lifecycle lane and a Spot execution gate turn. (#4771, #4784)
- Queued work that lost its execution right ends with NotFound regardless of cleanup order. Queued work of a stopped object is cancelled rather than failed. A ClientServer send waiting for Ready ends with ShuttingDown at shutdown. (#4371, #4567, #3977, #3979, #4030, #4036)
- Adds ownership handling during a Location Store outage. The owner lease TTL is 30 seconds and the release wait follows three margins. It includes the outage retention option and episode observation, the retention gate with discard after T, resuming preserved objects after recovery, reclaiming the lease only after the lease lifetime plus margin during an outage, a recorded lease admission deadline that blocks new handler turns, and handling a second outage as a new episode. (#3877, #3908, #3942, #3967, #3987, #4045, #4203, #4231)
- Adds the etcd Location Store provider (`Zlink.Framework.Locations.Etcd`). (#4079, #4102)

## Defect Fixes

- Context Close registers with the Spot lifecycle inside the call, and a joining wait follows caller cancellation. (#4497, #4505)
- A membership callback returns the Spot gate while waiting to retry. Suspended handlers no longer hold an application worker. (#4770, #4781, #4448, #4618)
- The startup-failure notification of a host that lost a fixed RID claim is delivered even if it arrives after generation cleanup. (#4635, #4636)
- Message wrappers created by the Framework are closed on every termination path. (#4590, #4602)
- The deadline index request path consumes the admission result instead of dropping its observer. The envelope header cache no longer waits synchronously on a lane from a handler context. Spot stop-source cancellation is serialized with its disposal. (#4456, #4469, #4460, #4485, #3877, #4004)
- The TicTacToe Configuration library is now a regular SDK project so it also builds on SDK 11. (#4438, #4479)

## Performance Improvements

- Handler result adapters are compiled into the invoker and the lane's synchronous turn takes one path, saving 40–80 B per call. (#4356, #4832)
- Idle state-lane posts run inline without a queued item. An unchanged RouteMesh status query no longer builds a candidate graph, and mesh status is projected once. (#4355, #4785, #4643, #4648, #4590, #4603)
- The default typed JSON no longer copies through an intermediate JSON array and temporary Base64 buffers. Duplicate allocations and length passes in the service wire head are removed, and repeated envelope header conversions are reduced. (#4354, #4418, #4353, #4467, #4434)
- Writes the request wire straight into native Messages, submits a ready node request in one selector and one operation turn, and adds prepared dispatch with immediate worker assignment and direct binding Reply, RouteMesh channel handlers running in the mailbox claim turn, and lower request deadline and reply decode overhead. (#3869, #3899, #3936, #3953, #3985, #3995)

## Dependencies

- `Zlink.Framework` now depends on Google.Protobuf 3.30.0. `Zlink.Framework.AspNetCore` also gains a Google.Protobuf 3.30.0 dependency, and Google.Protobuf in `Zlink.Framework.Codecs.Protobuf` and `Zlink.Framework.Locations.Etcd` moves from 3.29.1 to 3.30.0. (#4713)

## Install

```xml
<PackageReference Include="Zlink.Framework" Version="0.33.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.33.0" />
```

The release tag is [`framework-dotnet/v0.33.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.33.0).
