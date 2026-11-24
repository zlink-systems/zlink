[English](./framework-cpp-0.33.0.md) | [한국어](./framework-cpp-0.33.0.ko.md)

# ZLink C++ Framework 0.33.0 Release Notes

Framework 0.33.0 uses C++ binding 1.26.0 and Core 1.26.0. (#4846)

## Compatibility Changes

- A Spot or Actor handler owner type must equal the concrete registered type. A mismatch is a startup configuration error for both explicit and scan registration. Split a shared parent-type handler into per-type handlers and call the shared logic as an ordinary method. (#4548, #4617)
- A Spot can no longer call its own close inside its own lifecycle callbacks (Join, Leave and so on); the call ends with InvalidOperation at the call site. Register a short timer in the callback and call close from the timer handler without waiting on it. The Bingo samples (five languages) and the Node TicTacToe sample now do this. (#4432, #4709, #4801)
- The channel header moved from JSON to protobuf (proto3). Because the wire format changes, 0.33.0 nodes and 0.32.x or older nodes cannot share a mesh; a mixed hello is rejected (required capability framework-service-v13 → v14). Upgrade every node in the mesh together. (#4352, #4713)
- The server Framework license changes from zlink License 1.0 to Elastic License 2.0. Apache-2.0 for client connectors, engine adapters, http-client, samples and templates, and MPL-2.0 for Core and bindings, are unchanged. (#3928, #4697)

## Behavior Changes

- Core 1.26.0 makes `zlink_disconnect` end only connect registrations and `zlink_unbind` end only bind registrations. Disconnecting the previous descriptor endpoint of a replaced node no longer closes the listener and inbound connections of a new node bound to the same endpoint. (#4846, #4864)
- Status queries and observation of registered RouteMesh, ClientServer and automatic fanout now return a snapshot (`Starting`, IsReady=false) before and during runtime start. Previously each language ended with an error such as InvalidOperation. Readiness code that caught status exceptions during startup can be removed. (#4800, #4812)
- An Actor Join to a closed or inactive target Spot is rejected with Unavailable in all four languages. An Actor Join whose resolver confirmed a Closing authority ends with NotFound even if the activation disappears in between. (#4510, #4577, #4825, #4831)
- Caller cancellation and deadline results are published after local admission is returned, and an accepted request reacquires admission at its FIFO position before running. (#4286, #4676)
- Public calls end at one host admission point: InvalidOperation before start and ShuttingDown after stop. (#4205, #4265)
- A same-node bind also goes through the owner Actor's duplicate-bind decision, and the duplicate-bind callback runs on the Actor lifecycle lane and a Spot execution gate turn. (#4501, #4516, #4771, #4784)
- Runtime weight setters only record the value, and descriptor publication is left to the renew schedule. Calling a setter during the first-start release wait of a Fixed RID no longer fails host startup. (#4858, #4861)
- Instance Spot send and request failures record target_rid, reason and error detail in message flow. (#4291, #4712)
- A request with no owner server information yet waits for a discovery refresh within its remaining time. The Location authority scan narrows providers by request kind and aligns page limits and progress rules across the four languages. (#4160, #4307, #4260, #4720)
- An ID direct request addressed to an Actor recreated on the same owner is handled by the current Ready incarnation. (#4520, #4601)
- The Actor request deadline includes location lookup time, and a request that is terminal after lookup is not submitted. Actor create and Instance Spot wait for the authority generation, and request admission receives the owner generation the authority decided. (#4543, #4592, #4335, #4400, #4428)
- A remote Entry Spot join uses the existing canonical Actor join with the target descriptor's lifecycle generation. Expired-peer cleanup treats a disconnect NOT_FOUND as already terminated. (#4461, #4791, #4301, #4311)
- The owner lease admission deadline is recorded at reservation time and execution decisions read that record. New Actor placement candidates are chosen only from the live descriptor list. (#3877, #4216, #4205, #4217)
- Adds ownership handling during a Location Store outage. The owner lease TTL is 30 seconds and the release wait follows three margins. It includes the outage retention option and episode observation, the retention gate with discard after T, verifying and resuming preserved objects after recovery, reclaiming the lease only after normal expiry during an outage, and handling a second outage as a new episode. (#3877, #3909, #3946, #3975, #3993, #4020, #4029, #4048, #4055, #4058, #4068, #4269)
- Adds the etcd Location Store provider as an opt-in extension. (#4081, #4137)
- A renew that finds the lease stale outside an outage discards executions. (#4625)

## Defect Fixes

- Fixes a stall when opposite Joins between two Spots commit at the same time with message-flow tracing enabled. The actor transfer marker no longer waits on a lane or dispatcher. (#4857, #4865)
- A standalone host now terminates the Core context it created on close. (#4862, #4869)
- A ClientServer client that uses only manual endpoints without a Location Store is now admitted through hello. (#4845, #4847)
- The stream connector wait registers a packet that arrives while the queue is being checked instead of missing it. Stream write no longer re-awaits the transport completion task, so frames after shutdown do not hold the Stream host. (#4840, #4154, #4746)
- A manual fanout subscriber returns its worker when the handler suspends. Suspended handlers no longer hold an application worker, and record lifetime and Actor handoff completion are corrected. (#4772, #4786, #4448, #4618)
- The termination result is reused after run ends. Handlers, filters and channel owners are created per dispatch. Same-owner claims of Node and Channel run independently. (#4420, #4439, #4145, #4161, #1639, #4313)
- The generated ZLJR codec reads the length first and computes the end position so Clang no longer rejects the source. (#4275, #4365)
- The Redis Location Store snapshot scan decides expiry with the Redis clock, and later pages return the time of the first page. (#4067, #4655)

## Performance Improvements

- The default typed JSON now reads and writes through yyjson. Temporary string allocations in float and bytes encoding are reduced, and byte payload handling is streamlined. (#4354, #4826, #4427)
- Copies, string ownership and validation passes in envelope header writing, reading and decoding are reduced, keeping the L1→L2 cost within 7%. (#4352, #4443, #4466, #4513)
- Reduces submit allocations and copies on the request path, has one block own each receiver, and encodes the Node request header once into the service frame. Receive goes into one canonical record, and duplicate source-side send work is removed. (#3867, #3884, #3888, #3912, #3919, #3957, #4014, #4124, #4155, #4256)
- A contended source submit runs on the lane's existing drain. Completion callbacks run on a fixed two-worker pool, and target receive roles are assigned at rotation boundaries. (#4355, #4805, #1639, #4179, #4302)
- A Ready route cache miss reads the owner lease once and uses its absolute deadline as the cache lifetime. The per-record move allocation of claimed mailbox records and per-packet serial_log writes in STREAM dispatch are removed. (#4260, #4277, #4375, #4727, #4758, #4780)

## Dependencies

- The C++ Framework now uses yyjson 0.12.0 internally, with matching vcpkg and Conan versions. The public headers do not expose it; only static builds need yyjson at link time, and the installed CMake config finds it with `find_dependency(yyjson CONFIG)`. (#4354, #4826)
- Release builds and CI are pinned to the same grpc 1.84.0, abseil 20260107.1 and protobuf 6.33 line. (#4077, #4559)

## Install

Choose the platform Framework archive from the [GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.33.0). The server Framework uses C++ binding 1.26.0 and Core 1.26.0. Clients that use only the Stream Connector need neither Core nor the binding.

The release tag is [`framework-cpp/v0.33.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.33.0).
