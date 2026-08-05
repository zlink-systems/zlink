<!-- framework-adapter-nav:start -->
[E2E Index](README.en.md) | [Previous: Monitoring](config-7-monitoring.en.md) | [Next: Actor Messaging](config-9-to-actor-messaging.en.md)
<!-- framework-adapter-nav:end -->

# Config 8 — Async, Yield, And The Execution Turn

A Spot or Actor callback runs one at a time in its own execution lane. Waiting on a request or worker
result with `Async` keeps the current turn, while waiting with `Yield` in a `SpotWide` User Spot or
Instance Spot briefly gives back the shared Spot gate. Even after giving it back with `Yield`, the
member Actor's FIFO claim is kept, so the same Actor's next message does not run first.

This config runs real remote requests, timers, Actor mailboxes, and workers together to verify this
distinction as application evidence. It does not use scheduler thread IDs, private queues, or
test-only dispatch hooks.

## 1. Verification Scope

- The completion and turn meaning of one-way submit, `Async`, and `Yield`
- The execution order of Spot state, timers, and continuations
- Execution-lane separation between I/O workers and CPU workers
- Combinations of `SpotWide`, `PerActor`, and Actor FIFO
- Deferred Actor Join registered from a handler
- Remote topology, Session relay, and timeout/cancellation/Shutdown results
- The execution meaning that must be preserved even when the public representation differs per language

## 2. Deployment Configuration

| Role | Count | Purpose and reason for separation |
|---|---:|---|
| Location Store | 1 | Lets the two Object Servers and the Session gateway use the same Spot/Actor location. |
| Play node | 2 | Provides `SpotWide`/`PerActor` User Spots, an Entry Spot, and Actors. Records counters, timers, and callback sequences as application evidence. |
| Delay service | 2 | Replies to a public Channel request on an application signal or a specified deadline. Makes the await window deterministic. |
| External API | 1 | An HTTP server outside the Framework. Creates a condition where an I/O worker actually waits on external I/O. |
| Session gateway | 1 | Provides Stream Session and Actor binding, used to verify an Actor callback started through Session relay. |
| E2E client | 1 | Calls only the role servers' public application endpoint and Stream endpoint. |

Application evidence records the operation ID, callback kind, and start/completion sequence. It does
not turn Framework-internal identities such as `turn id` or `mailbox id` into new public evidence.
Whether execution overlaps is confirmed from the active count a handler increments/decrements on an
application counter, together with start/end order.

## 3. Common Run And Judgment Method

The runner creates a new Spot/Actor ID, operation ID, and evidence state for each scenario. An await
window is opened not by a fixed sleep but by the delay service's and handler's application signal.
Public timer configuration and a monotonic timestamp, plus runner tolerance, are used only when
verifying a timer's due boundary.

Each scenario starts after confirming the role servers' health and public target readiness. Request
terminal and handler evidence are checked together; file logs and scheduler timing are not used as
success conditions.

## 4. Scenarios

### Track A — Async Keeps The Current Turn

#### TD-A1 A One-Way Send's Completion Does Not Wait For The Handler To Finish

Priority: `P0`

A one-way send waits only up to outbound admission. If a send stayed pending until the remote handler
finished, it would carry the same completion meaning as a request.

**Verification question:** Does the send complete first even while the remote handler waits on an
application signal?

- Starting condition: The delay handler is configured to receive the marker and then wait, with no
  reply, until a release signal.
- Procedure: The play node submits a one-way send to the delay Channel. The send terminal is
  confirmed, handler evidence is read, and the release signal is sent.
- Verification: The send returns a normal terminal before the handler release. The handler processes
  the marker exactly once.
- Detailed behavior: verifies send completion in [Error Model §4](../spec/32-framework-error-model.en.md).

#### TD-A2 The Next Callback Of The Same Spot Does Not Start While Async Is Waiting

Priority: `P0`

`Async` keeps the Spot turn until the handler finishes. If the same Spot's next callback cut in, state
before and after the await could not be used safely.

**Verification question:** While an Async request is waiting, does the same Spot's probe callback not
start?

- Starting condition: The `TurnProbeSpot` counter is 0, and the delay request waits on the release
  signal.
- Procedure: Public evidence confirms the first handler is waiting on the delay request with `Async`.
  A probe request is sent to the same Spot, and the delay reply is released.
- Verification: The evidence order is `async-held, async-resumed, async-completed, probe-started,
  probe-completed`. The active callback count never exceeds 1.
- Detailed behavior: verifies [Async Execution Policy §1.1](../spec/05-async-execution-policy.en.md).

#### TD-A3 Read-Modify-Write Across An Async Window Is Preserved

Priority: `P0`

Because the turn is kept, even if multiple handlers read and wait on the same Spot counter, the next
handler reads the value only after the previous one finishes.

**Verification question:** Do N concurrent client requests, each going through an Async window,
increase the counter by exactly N?

- Starting condition: A Spot with counter 0 and a delay reply prepared per operation ID.
- Procedure: N distinct requests are started concurrently, and each handler reads the counter, then
  waits on the Async request. All delay replies are released.
- Verification: Every request receives one reply, and the final counter is N. The handler active
  count is always 1.
- Detailed behavior: verifies serial turn in [Async Execution Policy §2](../spec/05-async-execution-policy.en.md).

#### TD-A4 An Async Turn And A Remote Completion Do Not Block Each Other

Priority: `P0`

Even while a Spot turn is being kept, transport replies and infrastructure completion must still be
processed. Otherwise every Async request would deadlock on its own turn.

**Verification question:** Does a remote request that kept its Spot turn receive a reply and resume
normally?

- Starting condition: The delay service records public evidence immediately on receiving the
  request, then replies after a release signal.
- Procedure: The Spot handler waits on the request with Async. Remote receipt is confirmed, then the
  reply signal is sent.
- Verification: The handler resumes on the reply and finishes normally before the deadline. The same
  Spot's next callback runs after that.
- Detailed behavior: verifies [Async Execution Policy §3](../spec/05-async-execution-policy.en.md).

#### TD-A5 A Due Timer While Async Is Waiting Runs After The Handler

Priority: `P1`

Timer callbacks also use the same Spot turn. Even if a timer comes due while an Async handler is
keeping the turn, it must not overlap with the handler and must run only after the turn ends.

**Verification question:** If an Async handler waits past a timer's due time, does the timer callback
run after the handler finishes?

- Starting condition: A one-shot timer is registered, and the delay request waits on an application
  signal.
- Procedure: Once the handler is Async-held, the timer's due timestamp plus runner tolerance is
  confirmed to have passed. It is confirmed there is no timer evidence yet, and the delay reply is
  released.
- Verification: The evidence order is `async-held, async-completed, timer-started, timer-completed`,
  and the callback active count never exceeds 1.
- Detailed behavior: verifies [Async Execution Policy §5](../spec/05-async-execution-policy.en.md).

### Track B — Yield And Giving Back The Shared Spot Gate

#### TD-B1 A Callback Of The Same Spot Runs While Yield Is Waiting

Priority: `P0`

The reason to use `Yield` is to make progress on other work in the same Spot while waiting on a remote
operation.

**Verification question:** Does the same Spot's probe callback complete while a Yield request is
waiting?

- Starting condition: A `SpotWide` User Spot's delay request waits on the release signal.
- Procedure: Once the first handler is Yield-held, a probe request is sent to the same Spot. The
  probe reply is confirmed, then the delay reply is released.
- Verification: The evidence order is `yield-released, probe-started, probe-completed, yield-resumed,
  yield-completed`.
- Detailed behavior: verifies [Async Execution Policy §1.1](../spec/05-async-execution-policy.en.md).

#### TD-B2 A Yield Continuation Follows The Existing Queue Order

Priority: `P0`

Even once a remote reply arrives, the continuation is not run inline within the current callback. It
enters the shared gate's queue and resumes after a callback that was already waiting ahead of it.

**Verification question:** Does a probe that entered the queue earlier finish before the Yield
continuation?

- Starting condition: The first handler is Yield-held. The Probe 1 handler is configured to start
  and then wait on an application signal.
- Procedure: Probe 1 and Probe 2 are submitted in order. Once Probe 1 is confirmed running, the delay
  reply is released, making the Yield continuation ready. Finally, Probe 1 is released.
- Verification: The evidence order is `probe-1-started, probe-1-completed, probe-2-completed,
  yield-resumed`. The continuation's and the probe's active callback counts do not overlap.
- Detailed behavior: verifies [Async Execution Policy §3](../spec/05-async-execution-policy.en.md).

#### TD-B3 Shared State Is Re-Read After Yield

Priority: `P1`

Spot state read before a Yield can be changed by another callback. Instead of statistically checking
whether a "lost update happens to occur," the state-change order is pinned with an application signal
and verified.

**Verification question:** Does the continuation observe the value the second handler changed while
the first handler was Yielding?

- Starting condition: The counter is 10, and the first handler reads this value and then becomes
  Yield-held.
- Procedure: The second handler is confirmed to have changed the counter to 20 and finished. The
  delay reply is released, resuming the first continuation.
- Verification: The first continuation re-reads the current value, 20, and processes based on that
  value. It does not go on using the 10 it read before the Yield.
- Detailed behavior: verifies [Async Execution Policy §4](../spec/05-async-execution-policy.en.md).

#### TD-B4 A Timer Callback Runs While Yield Is Waiting

Priority: `P0`

Because Yield gives back the shared Spot gate, a due timer must not be pushed behind a remote request
wait.

**Verification question:** Does a one-shot timer run during a Yield-held window?

- Starting condition: A one-shot timer is registered on a `SpotWide` User Spot, and the delay reply
  is held.
- Procedure: Once the handler is Yield-held, timer evidence is polled with a bounded wait. After the
  timer completes, the delay reply is released.
- Verification: The evidence order is `yield-released, timer-started, timer-completed,
  yield-resumed`.
- Detailed behavior: verifies [Async Execution Policy §5](../spec/05-async-execution-policy.en.md).

### Track C — Separate Worker Kind From Spot Turn

#### TD-C1 Wait On An I/O Worker With Yield

Priority: `P0`

External HTTP I/O runs on an I/O worker, and waiting on the worker call with Yield lets the same
Spot's other callbacks and timers progress.

**Verification question:** Do the probe and timer complete, and does the I/O continuation resume
after them, while waiting on the External API?

- Starting condition: The External API reply is held on an application signal, and a Spot timer is
  registered.
- Procedure: The Spot handler starts an HTTP request through `RunIoWorker` and waits on the worker
  call with Yield. Probe and timer completion are confirmed, then the HTTP reply is released.
- Verification: The probe and timer finish before the I/O continuation, and the HTTP result is
  included in the original handler's reply.
- Detailed behavior: verifies the I/O worker in [Async Execution Policy §6](../spec/05-async-execution-policy.en.md).

#### TD-C2 Waiting On An I/O Worker With Async Keeps The Turn

Priority: `P1`

The worker kind does not automatically decide the turn meaning. Even the same I/O worker keeps the
current Spot turn when waited on with Async.

**Verification question:** Does the same Spot's probe not start while waiting on an I/O worker with
Async?

- Starting condition: The External API reply is held.
- Procedure: The Spot handler waits on the I/O worker with Async and submits a probe to the same
  Spot. The HTTP reply is released.
- Verification: The probe starts only after the I/O handler finishes. It is the worker call's
  terminator, not the External API request itself, that decides the turn.
- Detailed behavior: verifies [Async Execution Policy §6](../spec/05-async-execution-policy.en.md).

#### TD-C3 Waiting On I/O Does Not Use CPU Worker Capacity

Priority: `P0`

If asynchronous I/O occupies a CPU worker thread, concurrent external requests beyond the pool size
would produce unnecessary capacity errors.

**Verification question:** Do more I/O worker requests than the CPU pool size complete without
`CapacityExceeded`?

- Starting condition: The External API holds concurrent requests on an application signal, and the
  CPU pool size is fixed through public configuration.
- Procedure: I/O worker operations totaling four times the pool size are started. Once all have
  reached the remote API, the replies are released.
- Verification: Every operation receives a normal reply, and there is no `CapacityExceeded`. Another
  Spot's probe also completes during the wait.
- Detailed behavior: verifies worker pool separation in [Async Execution Policy §6](../spec/05-async-execution-policy.en.md).

#### TD-C4 The CPU Worker And The Terminator Role Stay Separate

Priority: `P1`

The CPU worker moves computation to a bounded pool, but whether the same Spot makes progress is
decided by Async or Yield.

**Verification question:** When the same CPU work is waited on with Async and with Yield, is the
worker result the same, with only the Spot callback order differing?

- Starting condition: The CPU worker holds computation completion until an application signal.
- Procedure: The same computation is run in an Async variant and a Yield variant, each submitting the
  same Spot probe.
- Verification: The computation result is the same in both variants. In Async, the probe runs after
  the worker handler; in Yield, the probe finishes before the worker continuation. A separate batch
  exceeding the pool limit ends in a bounded, public `CapacityExceeded`.
- Detailed behavior: verifies [Async Execution Policy §6](../spec/05-async-execution-policy.en.md).

#### TD-C5 CPU Worker Saturation Does Not Block An I/O Worker

Priority: `P1`

If the CPU pool and asynchronous I/O execution shared the same limit, heavy CPU computation would
also stall external I/O.

**Verification question:** Does an I/O worker request complete while the CPU worker uses up the full
pool capacity?

- Starting condition: CPU workers, up to the pool size, are made to wait on an application gate.
- Procedure: Once all CPU workers are confirmed active, a separate Spot calls the external API through
  an I/O worker. The I/O reply is confirmed, then the CPU gate is released.
- Verification: The I/O operation finishes normally before the CPU gate is released. The CPU
  operations, once the gate is released, each return exactly one terminal.
- Detailed behavior: verifies execution-resource separation in [Async Execution Policy §6](../spec/05-async-execution-policy.en.md).

### Track D — Distinguish SpotWide From PerActor Lanes

#### TD-D1 If A SpotWide Actor Yields, Other Actors And Spot Callbacks Progress

Priority: `P0`

A member Actor's Yield gives back only the User Spot's shared gate. So another Actor, a Spot direct
handler, and a timer can all progress.

**Verification question:** Do Actor B, the Spot handler, and a timer finish while Actor A is
Yield-held?

- Starting condition: Actors A and B are in the same `SpotWide` User Spot, and A's delay reply is
  held.
- Procedure: After A's handler Yields, a B request, a Spot request, and a one-shot timer are run.
  Once all finish, A's reply is released.
- Verification: The B, Spot, and timer evidence all appear before A's continuation, and the callback
  active count within the shared gate never exceeds 1.
- Detailed behavior: verifies [Async Execution Policy §7](../spec/05-async-execution-policy.en.md).

#### TD-D2 The Same Actor's Next Record Runs After The Yield Continuation

Priority: `P0`

Even if an Actor Yields, it keeps its own FIFO claim. If the same Actor's next message ran first, the
Actor's state order would break.

**Verification question:** Does Actor A's second request start only after the first Yield
continuation and handler finish?

- Starting condition: Actor A's first handler is Yield-held.
- Procedure: A second request is sent to the same Actor, then the first delay reply is released.
- Verification: The evidence order is `job1-start, job1-yield, job1-resume, job1-end, job2-start`, and
  the Actor handler's active count never exceeds 1.
- Detailed behavior: verifies [Async Execution Policy §7](../spec/05-async-execution-policy.en.md).

#### TD-D3 Callbacks Do Not Overlap During A Timer Overrun

Priority: `P0`

Even while a timer handler is waiting on a public async operation, the callback for the same timer key
must not run concurrently. If the next due overlaps, the Framework may skip a tick, coalesce with a
bounded catch-up, or delay the next tick according to the chosen overrun policy, but it must never let
callback execution windows overlap.

**Verification question:** While the first timer handler waits on an async operation, does the next
due not run the same timer callback concurrently, and does it follow the configured overrun policy?

- Starting condition: The repeating timer's period is set shorter than the public async operation the
  first callback waits on, and that operation's completion is held on an application gate.
  Application evidence records the timer key, callback generation, and delivery index.
- Procedure: Once the first callback's entry is confirmed, at least one due boundary is confirmed
  passed by monotonic deadline, and the operation-completion gate is released. Subsequent evidence
  for the same timer is then collected with a bounded wait.
- Verification: The active callback count for the same timer key is always 1. The collected
  delivery/scheduled indices follow the chosen overrun policy's skip, bounded catch-up, or
  delayed-next rule. After the timer is re-registered or canceled, no callback from the previous
  generation runs.
- Detailed behavior: verifies [Async Execution Policy §5](../spec/05-async-execution-policy.en.md).

#### TD-D4 A PerActor Async Blocks Only The Same Actor Lane

Priority: `P0`

A `PerActor` User Spot uses a separate FIFO lane per Actor. Even if Actor A waits with Async, Actor B
and separate timer lanes must progress.

**Verification question:** While Actor A is Async-held, do B and the timer complete, with only A's
next request waiting?

- Starting condition: Actors A and B are in the same `PerActor` User Spot, and A's delay reply is
  held.
- Procedure: Once A is Async-held, A's second request, a B request, and two distinct timers are run.
  B and timer evidence are confirmed, then A's reply is released.
- Verification: B and the timers finish before A, and A's second request starts only after the first
  A handler. The active count of the same lane never exceeds 1.
- Detailed behavior: verifies [Async Execution Policy §8](../spec/05-async-execution-policy.en.md).

#### TD-D5 Reject Yield In An Unsupported Context Before The Operation Is Submitted

Priority: `P0`

Yield is meaningful only in a `SpotWide` User Spot or Instance Spot, which can give back a shared gate.

**Verification question:** Is calling Yield outside an Entry Spot, PerActor, or an owner turn an
`InvalidOperation`?

- Starting condition: A public endpoint is prepared to start the same remote request and worker call
  in each context.
- Procedure: A Yield variant is run in an Entry Spot, an Entry Actor, a `PerActor` Actor, a Channel
  handler, and a caller outside an owner turn. The Async variant of the same call is also run.
- Verification: The Yield variants each end once with `InvalidOperation`, with no remote handler
  evidence. The Async variants run to the normal contract.
- Detailed behavior: verifies context validation in [Framework API §12](../spec/06-framework-api.en.md).

#### TD-D6 Reject An Awaited Request That Needs The Same Claim

Priority: `P0`

If the current callback's claim is needed by the same target, waiting on that request cannot
structurally complete. The Framework rejects it before submission, without waiting for the timeout.

**Verification question:** Does an Actor self-request or a same-gate Async request end in
`InvalidOperation`?

- Starting condition: An Actor and a `SpotWide` handler are configured to start a request that needs
  the same claim as themselves.
- Procedure: The Async and Yield variants of an Actor self-request, and the Spot same-gate Async
  variant, are run. For contrast, a self one-way send is also run.
- Verification: The awaited requests are `InvalidOperation`, with no target handler evidence. The
  one-way send is accepted into the FIFO and processed exactly once, after the current handler.
- Detailed behavior: verifies [Async Execution Policy §9](../spec/05-async-execution-policy.en.md).

### Track E — Start An Actor Join Registered By A Handler Only After The Terminal

#### TD-E1 A Deferred Join Of An Entry Spot Actor Runs After The Handler

Priority: `P0`

Calling `Defer()` in a handler only registers a Join intent. The membership move must start only
after the handler finishes normally.

**Verification question:** Is the Actor in the Entry Spot before the handler terminal, moving to the
target User Spot after it?

- Starting condition: An Actor is in an Entry Spot, and the target User Spot is on the same node. The
  handler terminal is held on an application signal.
- Procedure: The Actor handler defers the Join, leaves `deferred` evidence, and waits. A public
  current-Spot lookup confirms the Actor is in Entry, then the handler is released.
- Verification: Before the handler terminal, there is no Join callback, and the current Spot is
  Entry. After it, the target `OnActorJoin`, `OnJoinedActor`, source `OnLeaveActor`, and the Actor's
  completion callback each run exactly once, and the current Spot is the target.
- Detailed behavior: verifies [Spot Actor §4](../spec/15-spot-actor.en.md#4-actor-join-and-commit-order).

#### TD-E2 PerActor And SpotWide Use The Same Deferred-Join Meaning

Priority: `P0`

Even if the source Spot's execution mode differs, Defer keeps the Actor claim until the handler
terminal. Even if an earlier request in SpotWide Yielded and resumed, the last handler terminal is
still the reference point.

**Verification question:** Do both execution modes start the Join callback only after the handler
terminal?

- Starting condition: One Actor each is placed in a `PerActor` and a `SpotWide` source Spot, with the
  target Spot on the same node.
- Procedure: Both handlers defer their Join, then wait on an application signal. The SpotWide variant
  Yields once earlier and resumes before deferring. The current Spot is confirmed before the
  terminal, then both handlers are released.
- Verification: Before the terminal, both Actors are in the source. After it, each Actor's target
  callback and completion callback run once, and it switches to the target's current Spot.
- Detailed behavior: verifies [Spot Actor §4](../spec/15-spot-actor.en.md#4-actor-join-and-commit-order).

#### TD-E2A A Registered Join Is Discarded On Handler Failure

Priority: `P0`

Even if one handler deferred Joins for multiple Actors, if the handler ends in an exception or
cancellation, every intent that hasn't yet been activated must be discarded.

**Verification question:** Do neither of the two Joins deferred by a failed handler start, keeping
existing membership?

- Starting condition: Actors A and B are in the source Spot, and the handler defers both Joins in
  turn.
- Procedure: An exception variant and a cancellation variant are each run on a fresh fixture. After
  the handler terminal, a source Spot request is sent to both Actors.
- Verification: There is no target/source Join lifecycle callback and no Actor completion callback.
  The public current Spot for both Actors is the source, and follow-up requests are processed
  normally.
- Detailed behavior: verifies the handler terminal in [Async Execution Policy §10](../spec/05-async-execution-policy.en.md).

#### TD-E3 Two Opposite-Direction Local Joins Progress Together

Priority: `P0`

Running local Joins for different Actor/Spot pairs only one at a time, node-wide, would block unrelated
moves too.

**Verification question:** Do an Actor moving from A to B and an Actor moving from B to A both move
within the deadline?

- Starting condition: Actor X is in Spot A, Actor Y is in Spot B, and both handler terminals can be
  released by the same application barrier.
- Procedure: Both Actor handlers defer opposite-direction Joins, and once both are confirmed
  registered, the barrier is released.
- Verification: Both completion callbacks are Accepted, and the public current Spots swap. Each
  Actor's callback runs exactly once, with no timeout.
- Detailed behavior: verifies per-Actor independence in [Spot Actor §4](../spec/15-spot-actor.en.md#4-actor-join-and-commit-order).

### Track F — Keep The Same Meaning Even Over Remote Paths And Terminal Failures

#### TD-F1 Async And Yield Mean The Same Even For A Remote Spot Request

Priority: `P0`

Even if the target Spot is on a different node, the source Spot's turn-management meaning does not
change.

**Verification question:** Does a remote request's Async variant block the probe, and its Yield
variant let the probe progress?

- Starting condition: The source Spot is ready on `play-a`, and the delay target Spot is ready on
  `play-b`.
- Procedure: The TD-A2 and TD-B1 application-signal procedures are each repeated as a remote Spot
  request.
- Verification: The Async evidence has the probe starting after the source handler, and the Yield
  evidence has the continuation resuming after the probe. Both remote requests return one reply.
- Detailed behavior: verifies [Async Execution Policy §2](../spec/05-async-execution-policy.en.md).

#### TD-F2 The Same Meaning Applies Even Starting From A Channel Handler

Priority: `P1`

A Channel handler invoked through RouteMesh also does not change the public request terminator's
meaning.

**Verification question:** When a Channel handler waits on a Spot request, do Async's and Yield's
context validation and order match the contract?

- Starting condition: A remote caller and a Channel handler are ready.
- Procedure: The Async and Yield variants of the same Spot request are each run from the handler.
- Verification: A context that does not support Yield, like a Channel handler, is `InvalidOperation`
  with no remote Spot handler run. The Async variant returns a normal reply.
- Detailed behavior: verifies [Framework API §12](../spec/06-framework-api.en.md).

#### TD-F3 The Same Meaning Applies Even For An Actor Handler Started Through Session Relay

Priority: `P1`

Even if an Actor packet comes in through Stream Session relay, the Actor mailbox and Spot gate
contract are the same.

**Verification question:** Do SpotWide Yield and Actor FIFO still hold for an Actor request started
by a bound Session?

- Starting condition: A Stream Session is bound to Actor A in a `SpotWide` User Spot.
- Procedure: The client sends a relay packet to make A's handler Yield-held, then sends A's next
  packet and an Actor B packet on the same Session. The delay reply is released.
- Verification: The B packet is processed during the Yield window, and A's next packet is processed
  after the first A handler finishes.
- Detailed behavior: verifies [Session Actor Dispatch §6](../spec/20-session-actor-dispatch.en.md#6-failure-handling)
  and [Async Execution Policy §7](../spec/05-async-execution-policy.en.md).

#### TD-F4 The Spot Turn Is Returned After A Timeout

Priority: `P0`

Even if an awaited request times out, the current turn or shared gate must not remain locked.

**Verification question:** Do Async and Yield requests process a follow-up Spot request after
`DeadlineExceeded`?

- Starting condition: The delay service is configured not to reply to that operation ID.
- Procedure: The Async and Yield variants are each run on a fresh Spot, and the public deadline
  terminal is awaited. A probe request follows.
- Verification: Both variants end in exactly one `DeadlineExceeded` terminal, and the probe receives
  a normal reply.
- Detailed behavior: verifies [Error Model §5](../spec/32-framework-error-model.en.md).

#### TD-F5 The Owner Keeps Being Usable After Waiter Cancellation

Priority: `P1`

A caller canceling its await does not mean canceling an operation already accepted remotely, or the
owner's lifecycle.

**Verification question:** Does a new request to the same Spot/Actor process normally after waiter
cancellation?

- Starting condition: A remote handler accepts a delay request and holds the reply.
- Procedure: An Async or Yield waiter is ended with a public cancellation, then a new request is sent
  to the same owner. The remote reply is released last.
- Verification: The first awaitable returns one language-specific cancellation result. The follow-up
  request receives a normal reply, and the late reply does not complete a new operation.
- Detailed behavior: verifies [Async Execution Policy §3](../spec/05-async-execution-policy.en.md).

#### TD-F5A Start A Host Shutdown While Awaiting

Priority: `P1`

Shutdown closes new admission and cleans up already-accepted callbacks within the host deadline.

**Verification question:** After the shutdown seal, is a new operation rejected, and does an existing
await end in exactly one terminal?

- Starting condition: A delay request has been accepted remotely, and the source handler is
  awaiting.
- Procedure: A public Shutdown is started on the source host. Once host status shows it is not
  accepting new work, a new request is sent to the same owner, and the delay reply is released.
- Verification: The new request is `ShuttingDown`. The existing await ends exactly once, in either a
  reply or a shutdown-deadline result, and the host reaches a bounded terminal state.
- Detailed behavior: verifies [Graceful Drain §5](../spec/28-graceful-drain-handoff.en.md).

#### TD-F6 Reject A Wait-For Cycle Before The Timeout

Priority: `P1`

Waiting with Async on a self-request that needs the current Spot claim means the target handler can
never start.

**Verification question:** Does a Spot self-request Async end in `InvalidOperation`, with a follow-up
request succeeding?

- Starting condition: Spot A's handler is configured to start a request to itself.
- Procedure: The self-request Async variant is run, then a separate caller sends a probe request to
  A.
- Verification: The self-request is `InvalidOperation`, with no nested target handler evidence. The
  probe receives a normal reply.
- Detailed behavior: verifies [Async Execution Policy §9](../spec/05-async-execution-policy.en.md).

### Track G — Confirm The Same Execution Meaning Across Languages

#### TD-G1 Cross-Language Source And Target Also Produce The Same Order

Priority: `P0`

Even though the terminator method name and await syntax differ per language, the callback order seen
across processes must be the same.

**Verification question:** Are the Async and Yield evidence the same across source/target
combinations of different Framework languages?

- Starting condition: A play node and delay service in at least two languages are ready with the
  same packet contract.
- Procedure: TD-A2 and TD-B1 are run across every supported bidirectional language combination.
- Verification: In Async, the probe runs after the source handler; in Yield, the continuation resumes
  after the probe. Payload and terminal-error meaning are also the same across language
  combinations.
- Detailed behavior: verifies language parity in [Public Contract Governance](../spec/00-public-contract-governance.en.md).

## 5. Completion Criteria

- Every scenario uses only public request/send, timer, worker, Join, and application evidence from
  the role servers.
- The await window is controlled by application signals; callback interleaving is not inferred from
  a fixed delay.
- A Yield-driven state change is verified by TD-B3's deterministic state change, not as a "may
  happen" probabilistic condition.
- The active callback count of the same execution lane never exceeds 1, and a request has exactly
  one terminal result.
- API shape searches, blocking-call searches, and private scheduler inspection are not part of the
  E2E completion condition.
