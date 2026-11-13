# Gate-less owner multi-worker drain — spec review

**Verdict: reject the current diff.** The multi-worker design is viable, but the stated FIFO handler-start guarantee does not follow from a FIFO queue take. The diff also leaves the idle-worker wake protocol and the scheduler occupation bound insufficiently specified. These require contract decisions before implementation, rather than language-specific interpretations.

The replacement text below describes a coherent revision that preserves **FIFO record selection**, permits unordered application pipeline entry, and keeps the existing gated execution contracts. Adopting that revision explicitly changes the existing pipeline-entry ordering contract. If preserving that contract is required, do not adopt E1/E2: retain an ordered pipeline-entry mechanism and reassess its cost. Merely adding a FIFO pop does not preserve it.

Reviewed `/root/jobs/fx/cx/v21/s033/specrev/spec.diff`, the working-tree spec, `fdesign/design.md` §3.3–3.4, and `compare/compare.md`. The supplied diff exactly matched `git diff` at review time (SHA-256 `fb6b5c1e0fee1ceeeaf7c380bc39c1cb95e79ab821a5a5233b51c32a07581ec9`). All spec paths below are relative to `framework/doc/framework/common/spec/server/`; line numbers refer to this uncommitted snapshot. The worktree was not edited. No sub-agents, builds, tests, or benchmarks were run. Execution examples below are reasoning about allowed schedules, not measured reproductions.

## Findings

### R1 — Blocking: FIFO dequeue does not preserve the existing observable start order

Changed `01-execution/08-messaging-hot-path.ko.md:229–235` / `.en.md:252–258` says that taking the queue head fixes handler start order. Existing `01-execution/02-handler-turn-and-execution-gate.ko.md:98–104` / `.en.md:102–109` defines start as **entry into the filter/handler pipeline**, in receive order. These are different events.

Allowed schedule under the proposed queue algorithm:

```text
queue = [A, B]
worker 1 takes A, then is descheduled before W2/W3
worker 2 takes B, finishes W2, enters B's first filter/handler
worker 1 resumes and enters A's first filter/handler
```

There need be no suspension in either application callback. Different decode or DI-construction times can cause the same inversion without OS preemption. The queue takes are FIFO; pipeline entry is B, A. An application can observe this with an atomic append at the first filter instruction. With no filters, it can observe it at the first handler instruction.

Async overlap already permitted by §02 does not resolve this conflict: it allows a later pipeline to enter while the earlier pipeline is suspended, after the earlier pipeline has entered. It does not permit B to enter before A. The glossary delegates handler-turn boundaries to §02 (`00-foundation/02-glossary.ko.md:922–935` / `.en.md:973–986`); relabeling the dequeue as a handler start would also misrepresent that execution interval.

Two alternatives were considered:

- Preserve FIFO pipeline entry through an ordered invocation mechanism. Its contract must order actual application entry, not just a counter immediately before an invocation. Simply invoking synchronous callbacks inside one serial launch section also serializes their synchronous execution and does not achieve the proposed drain behavior.
- Explicitly guarantee FIFO record takes only, with no order guarantee for application entry. This permits the intended implementation without adding a second launch-order mechanism. **Recommended revision, subject to the supervisor accepting the observable contract change: E1 and E2.**

### R2 — Blocking: ready-state recheck after waking is not a no-missed-wakeup protocol

Changed `01-execution/04-application-job-queue-and-backpressure.ko.md:192–200` / `.en.md:213–223` only requires a *woken* resource to recheck. New §08 (`.ko.md:233–234` / `.en.md:256–257`) suppresses wakes unless an idle worker exists. Without a defined wait-registration boundary, all workers can sleep with work ready:

```text
workers observe an empty ready set, but have not registered as idle waiters
producer publishes work and sees no idle waiter, so sends no wake
workers register as idle and park without another ready-set check
ready work remains; there need not be another producer or management event
```

There is also a same-owner scale-out race: a second worker decides to park while the first worker is running a synchronous callback; a new record arrives while the second worker is not yet counted as idle. Rechecking only on a future enqueue or dequeue does not ensure the idle worker participates. The pending work must also be considered when a worker becomes available.

The phrase “never once per record” must prohibit **unconditional redundant record-triggered notifications**, not a numerical equality. Sparse singleton arrivals legitimately need one wake each. A batch of N records and N idle workers can also legitimately cause N worker activations. The first worker must be scheduled even if taking the sole record leaves no records for an *additional* worker.

The ready set must remain the scheduling authority, with notification/wait registration coordinated with that durable state. A condition-variable predicate protocol, retained notification, or equivalent runtime scheduling protocol may implement this; a separate per-record notification queue is unnecessary. **E3 and E4** specify the required race outcome, distinguish initial scheduling from additional activation, and coalesce wakes by worker availability.

### R3 — Blocking: the owner occupation bound has no defined multi-worker meaning

`00-foundation/06-framework-api.ko.md:547–550` / `.en.md:621–625` says one owner occupies the scheduler for a bounded interval, then hands execution to another ready owner, and says this establishes the other owners' maximum wait. `01-execution/02-handler-turn-and-execution-gate.ko.md:398–406` / `.en.md:456–469` defines a batch budget around one processing authority. New §08 allows several independent drains of that owner but does not define their budget unit.

An implementation could sum all workers' CPU time, share one owner's wall-clock deadline, or restart a budget at every take of the already-ready owner. These produce different behavior. In particular, keeping an owner continuously ready and resetting the budget on every W1 can let it repeatedly occupy all workers while another owner waits.

For a gate-less owner, use the existing budget independently for each worker's consecutive service of that owner. A worker must not reset that slice by reselecting the same owner. On expiry, another ready owner gets an opportunity before that worker continues the expired owner. This is cooperative fairness, not a hard node-wide latency bound: number of ready owners, resource availability, and non-preemptible callbacks still matter. Already-running handlers are not stopped, and time in a suspended handler is not worker occupation. **E6** defines this without introducing a global per-owner budget counter or timer, and explicitly preserves the gated-owner paragraph. Its gate-less maximum-wait wording is a contract revision too: it must be accepted as cooperative fairness rather than presented as an unchanged hard bound.

### R4 — Required clarification: early record take must not become permit release or quiescence

New §08 takes a record before W2. `01-execution/04-application-job-queue-and-backpressure.ko.md:120–123` / `.en.md:134–137` currently combines leaving the queue and returning the permit immediately before the actual handler instruction. An early physical dequeue must be distinguished from departure from the accounted Application job queue.

Otherwise several workers can remove records, return permits early, and leave unaccounted records in decode/scope preparation. This conflicts with §04's accounting (`.en.md:37–62`), the glossary (`00-foundation/02-glossary.en.md:1097–1106`), and queue metrics (`06-observability/02-runtime-metrics.en.md:120–139`). Keep the existing callback-start release boundary, or the pre-callback terminal release; keep taken-but-not-started jobs in `queued` accounting. **E5** makes the distinction explicit.

Likewise, an empty physical queue or absence from the ready set is not quiescence. Every taken record still has exactly one dispatch owner until terminal handling and cleanup. Normal shutdown must include all such records; a dispatch's scope cannot be disposed because a different worker finished. These are existing lifetime requirements, not permission for new shutdown rules.

### R5 — Local wording conflict: §4.1 counts switches “per owner” but measures them per record

`01-execution/08-messaging-hot-path.ko.md:187` / `.en.md:204` says the Framework introduces at most one resource switch **per owner**. The surrounding normative measurement interval is from one record's claim to that record's first handler instruction (`.en.md:206–212`). A batch for one owner distributed to several workers has several switches in aggregate but at most one per record. **E7** changes the table's unit to match the existing measured interval. It does not authorize a second switch for an individual record.

### R6 — Validation gap: existing §7 does not establish the new semantics

`01-execution/08-messaging-hot-path.ko.md:376–405` / `.en.md:415–449` checks receive batching and throughput, but has no direct check of the new same-owner behavior. Request-window in-flight count can be high using async overlap alone; it cannot distinguish multiple synchronous drains. Add public behavior checks and internal scheduling checks as **E8**. These are proposed implementation acceptance criteria; none was run for this read-only review.

## Application-visible effects and other clauses

| Surface / clause | Review result |
|---|---|
| Synchronous gate-less handlers | They can now execute on different application workers before either suspends. This is a real change from the described single-drain implementation, even though §02 already assigns synchronization of shared application state to the application. The existing explicit FIFO pipeline-entry guarantee is the contractual conflict in R1. Do not describe the entire change as observationally equivalent to async overlap. |
| Filters | Order **within one dispatch** remains registration order before `next`, reverse order after it; at-most-once `next`, rejection, and failure classification are unchanged (`00-foundation/06-framework-api.ko.md:468–498` / `.en.md:534–569`). Across dispatches, E1 permits the first filter for B to enter before A, including when all filters are synchronous. This is the public ordering change; filters must not accidentally run under a new owner-wide execution lock. |
| DI and instance lifetime | Each dispatch keeps a fresh handler/filter scope, sharing scoped dependencies only within that dispatch (`00-foundation/06-framework-api.ko.md:485–489` / `.en.md:553–558`; `01-execution/08-messaging-hot-path.en.md:274–307`). Concurrent scope preparation/disposal can now occur for synchronous dispatches. Sharing a scope or handler instance per worker/batch/owner to save allocation is forbidden. Application-shared services still require application synchronization. Existing constant-time preparation and no-extra-stage restrictions remain. No DI spec edit is needed. |
| Metrics and tracing | Queue pressure, request latency, and in-flight values can change. Their units, labels, accounting boundaries, and exactly-once terminal outcomes do not (`06-observability/02-runtime-metrics.en.md:120–145,188–203`). Preserve charged pre-start records as in E5. Metrics must not impose an ordering barrier (`.en.md:449–456`). Do not reinterpret queue-pop timestamps as actual callback-start timestamps or add a per-turn/per-dequeue metric. |
| Replies over one connection | W4 still encodes and submits each reply in its own handler turn, without an extra Framework queue or resource switch (`01-execution/08-messaging-hot-path.en.md:245–246`). For gate-less A then B, B may now submit its reply first even if both handlers are synchronous. General completion order was already unconstrained by async overlap; this change expands when reordering can occur. Binding/Core submit and transport contracts still apply; application drain concurrency does not authorize interleaving logical reply frames or implementing new Framework reply routing. |
| ClientServer reply order | Correlation identifies the original request (`02-channel-transport/03-client-server-channel.en.md:372–380,407–410`); there is no request-receipt-order reply guarantee here. Its single connection FIFO orders traffic actually submitted on that connection. It can delay a submitted reply behind preceding DATA (`.en.md:382–387`; `01-execution/04-application-job-queue-and-backpressure.en.md:182–187`; `01-execution/08-messaging-hot-path.en.md:378–379`). B being submitted before A does not violate that FIFO. Do not add a reply reorder buffer. |
| §02 gated execution and §07 serial executors | Unchanged. Spot/Actor ordinary async handlers retain their applicable gate; `Yield` retains/releases claims as already specified. `01-execution/07-serial-executor-layers.en.md:18–38,96–107` scopes its serial units to Spots, Actors, timers, and sessions. Its prohibition on starting a second drain (`.en.md:320–350`) must **not** be copied onto gate-less Node/ChannelName dispatch. Conversely, the new multi-drain rule must not be applied to those serial units. No §07 edit is needed. |
| §06 state lanes | No permission to run several state-lane turns concurrently. Queue-head ownership, ready membership, enqueue, and wait registration must satisfy one scheduling state authority, using the existing state-classification rules (`01-execution/06-state-ownership-and-lanes.ko.md:94–101` / `.en.md:101–112,176–187`). Handler execution happens outside that brief state-management span. Adding unrelated per-worker readiness flags or another owner-level handler gate would duplicate decisions. E3 states the boundary, not a new lane or public API. |
| §08 §4.1 / §4.4 | I0–I4, pre-receive permits, header-only classification, management once per receive turn, and initial ready publication remain. E7 fixes the switch-count unit. All extra workers are existing application resources: a receive-role holder may execute application work only after successful role handoff (`01-execution/08-messaging-hot-path.en.md:324–351`). A thread blocked in a receive wait, or reserved for the next receive role, is not an idle application worker. No additional poller, resource limit, or receive handoff per record is allowed. |
| Foundation glossary | Execution gate and handler turn defer to §02 (`00-foundation/02-glossary.en.md:958–986`). Keep actual execution as the handler-turn start; queue take is an internal scheduling event. No glossary edit is needed after E1. Application job queue still holds its permit until actual callback start, as E5 clarifies. |
| Cancellation and shutdown | No change to the first-terminal winner or binding ownership (`01-execution/03-cancellation-and-shutdown.en.md:64–78`). Cancellation of one record does not cancel another worker's record. Registered waits/scopes are cleaned per turn (`.en.md:139–158`). Accepted work includes concurrently running and taken-but-not-started records, not only the physical queue. |
| Relocation | Source payload/relay/cutover ordering still leads into the existing Actor/Spot serial rules (`05-location-relocation/04-relocation-flow.en.md:469–481`). Host admission seals, accepted-work drain, and resource cleanup remain owned by `05-host-relocation-flow.en.md:807–855`. Parallel Node/ChannelName callbacks must not parallelize Actor/Spot dispatch, bypass a seal, or make queue emptiness sufficient for transport teardown. No relocation edit is needed. |
| Sessions | STREAM callbacks retain their serial queue/gate (`04-session/01-stream-session.en.md:57–66,365–369`; `01-execution/07-serial-executor-layers.en.md:104–107`). Session binding and route changes remain one serial span (`04-session/02-session-actor-binding.en.md:65–76`). Hosting the session transport in a Node does not make a Session a gate-less Node handler. No session edit is needed. |
| Cross-language execution | C++, .NET, and Java can use several existing application workers. Node has one event loop, so synchronous callbacks do not run in CPU parallel; overlap is across async suspension, and its macrotask budget yield remains required (`01-execution/08-messaging-hot-path.ko.md:361–371` / `.en.md:399–409`). The common guarantee should permit concurrency, not demand two physical workers in every runtime. All languages must implement the same selection, ownership, accounting, lifetime, and fairness contracts; physical schedules need not be identical. |
| §08 §7 performance lines | Unchanged. The stated 0.6 design objective is not the normative pass line. §7(c) retains 0.90 for its listed cells and explicitly distinguishes request-backpressure's bench 0.80 (`.ko.md:388–393` / `.en.md:429–435`). Nothing reviewed establishes a performance result or justifies changing those thresholds. |

## Exact replacement text

These edits form one proposed revision. They are text proposals in this report only. E1 is an explicit public ordering change; the supervisor must accept that choice before treating the revision as approved. Preserve existing headings and anchors unless the edit explicitly includes them.

### E1 — §02: define FIFO at record selection, keep handler turn at actual pipeline entry

Replace the entire Node/ChannelName bullet at `01-execution/02-handler-turn-and-execution-gate.ko.md:98–104` / `.en.md:102–109`.

**ko**

```markdown
- **Node handler와 [ChannelName](../00-foundation/02-glossary.ko.md#channelname) handler에는
  execution gate가 없다.** Handler turn은 record마다 독립이다. 이 handler는 상태를 갖지
  않으며, 유지할 상태는 Spot·Actor가 소유한다. Worker는 owner queue에서 record를 FIFO
  순서로 꺼내지만, record별 filter·handler pipeline과 handler 본문에 진입하는 순서는
  정하지 않는다. Queue에서 꺼내는 동작은 handler turn의 시작이 아니며, handler turn은
  해당 record의 filter·handler pipeline에 실제로 진입할 때 시작한다. 다음 record의
  pipeline은 앞 handler가 suspend하거나 완료하기 전에도 다른 application worker에서
  실행될 수 있다. Handler 사이에서 공유하는 application 상태의 동기화는 application이
  소유한다.
```

**en**

```markdown
- **Node handlers and [ChannelName](../00-foundation/02-glossary.en.md#channelname) handlers have no
  execution gate.** Each record's handler turn is independent. These handlers hold no state; state
  that must persist is owned by Spots and Actors. Workers take records from the owner queue in FIFO
  order, but the order of entry into each record's filter and handler pipeline and into handler
  bodies is not defined. Taking a record from the queue does not start its handler turn; the turn
  starts at actual entry into that record's filter and handler pipeline. The next record's pipeline
  may execute on another application worker before the preceding handler suspends or completes.
  The application owns synchronization of application state shared between handlers.
```

### E2 — §08 §4.3: distinguish record ownership from owner exclusivity

Replace the W1 row at `.ko.md:220` / `.en.md:243`.

**ko**

```markdown
| W1 acquire | 준비된 owner 집합에서 owner를 고른다. Execution gate가 있는 owner면 그 [execution gate](../00-foundation/02-glossary.ko.md#execution-gate)를 잡는다([Handler turn 「6」](02-handler-turn-and-execution-gate.ko.md#6-처리-권한-획득의-함정-구현), [「11」](02-handler-turn-and-execution-gate.ko.md#11-두-동기화-지점을-싸게-만든다-구현)). Gate가 없는 Node·ChannelName owner면 §4.3 아래의 규칙에 따라 record 하나의 실행 소유권을 얻는다. | Gate가 있는 owner의 무경합 gate 획득은 원자 연산 하나다. Gate가 없는 owner는 record별 실행 소유권을 사용한다. |
```

**en**

```markdown
| W1 acquire | Selects an owner from the ready set. If the owner has an [execution gate](../00-foundation/02-glossary.en.md#execution-gate), acquires it ([Handler Turn "6"](02-handler-turn-and-execution-gate.en.md#6-the-trap-in-acquiring-processing-authority-implementation), ["11"](02-handler-turn-and-execution-gate.en.md#11-making-the-two-synchronization-points-cheap-implementation)). For a Node or ChannelName owner without a gate, acquires execution ownership of one record under the rules below. | An uncontended gate acquisition takes one atomic operation. An owner without a gate uses record-level execution ownership. |
```

Replace the paragraphs from `.ko.md:226–238` / `.en.md:249–261`, ending before the bold prohibition on per-record tasks.

**ko**

```markdown
Gate가 없는 Node·ChannelName owner의 record 선택 순서와 handler 실행 순서는
[Handler turn §2](02-handler-turn-and-execution-gate.ko.md#execution-gate)를 따른다.

**Gate가 없는 Node·ChannelName owner는 여러 application worker가 처리할 수 있다.**
Worker는 W1에서 owner queue head의 record 하나에 대한 실행 소유권만 얻으며, owner
전체를 독점하지 않는다. 다른 worker가 다음 record를 얻는 데 앞 handler의 suspension이나
terminal completion을 요구하지 않는다. Queue에서 record를 꺼내고 ready 상태를 갱신하는
절차와 worker를 깨우는 조건은
[Application job queue §3의 준비된 owner 집합](04-application-job-queue-and-backpressure.ko.md#준비된-owner-집합-구현)이
소유한다. Record를 꺼낸 worker는 W2~W4와 그 record의 terminal 정리를 책임진다.
Handler가 suspend한 뒤에는 기존 continuation 경로가 그 책임을 이어받는다. Queue가
비었거나 owner가 ready 집합에 없다는 사실만으로 진행 중인 dispatch가 끝난 것은 아니다.

Gate가 없는 handler가 W3에서 suspend하면 worker는 W5로 간다. 그 handler의 W4 reply는
handler가 완료된 뒤 해당 handler의 turn 문맥에서 실행한다. Gate가 있는 owner의 gate·claim
범위와 일반 비동기 대기 및 `Yield`의 권한 유지·이전은 Handler turn §2·§3을 따른다.
```

**en**

```markdown
Record selection and handler execution order for a Node or ChannelName owner without a gate follow
[Handler Turn §2](02-handler-turn-and-execution-gate.en.md#execution-gate).

**Several application workers may process a Node or ChannelName owner without a gate.** In W1 a
worker acquires execution ownership of one record at the owner-queue head, not exclusive ownership
of the whole owner. Another worker's take of the next record does not require the preceding handler
to suspend or reach terminal completion. The procedure for taking a record and updating readiness,
and the conditions for waking workers, are owned by
[the set of ready owners in Application Job Queue §3](04-application-job-queue-and-backpressure.en.md#the-set-of-ready-owners-implementation).
The worker taking the record is responsible for W2–W4 and that record's terminal cleanup. After
handler suspension, the existing continuation path carries that responsibility. An empty queue or
absence from the ready set does not by itself mean that in-progress dispatches have finished.

When a handler without a gate suspends in W3, the worker proceeds to W5. That handler's W4 reply
runs after it completes, in its handler-turn context. For owners with a gate, gate and claim scope,
and authority retention or transfer across ordinary asynchronous waits and `Yield`, follow Handler
Turn §2–§3.
```

The existing W5 row can remain: E6 supplies its precise budget meaning. Do not read “the worker holding the gate” as thread affinity across asynchronous suspension.

### E3 — §04 ready set: make membership and the wait protocol normative

Replace the text under “준비된 owner 집합 (구현)” / “The Set of Ready Owners (Implementation)” through its internal confirmation condition (`.ko.md:189–200` / `.en.md:210–223`).

**ko**

```markdown
Permit을 얻어 owner queue에 들어온 record의 실행 가능 여부는 준비된 owner 집합으로
나타낸다. 같은 owner는 집합에 한 번만 들어 있다.

Gate가 없는 Node·ChannelName owner는 아직 worker가 꺼내지 않은 실행 가능한 record가
남아 있는 동안 집합에 남는다. 이미 다른 worker가 그 owner의 handler를 실행 중이라는
이유로 제외하지 않는다. Record를 꺼낼 때는 queue head의 record 하나를 한 worker에만
넘기고, 남은 record에 맞게 집합의 membership을 갱신한다. Enqueue, 마지막 record를 꺼낼
때의 제거, 새 record가 들어올 때의 재등록은 같은 scheduling 상태 소유자가 하나의 순서로
확정한다. Queue head의 record 선택은 그 owner를 독점할 권한을 주지 않는다. Gate가 있는
owner의 실행 가능 여부는 기존 gate·claim 규칙을 따른다.

Worker는 대기 알림을 받을 수 있는 상태로 등록한 뒤 ready 집합을 다시 확인하고, 처리할
일이 없을 때만 실제로 기다린다. Ready 상태의 게시와 대기 전환은 게시자가 대기 worker를
확인하거나 대기하려는 worker가 게시된 작업을 확인하도록 조정한다. 깨어난 worker도 집합을
다시 확인한다. 일시적인 알림의 유실이나 게시와 대기의 경쟁 때문에 실행 가능한 작업을
남긴 채 사용 가능한 모든 worker가 계속 대기하는 상태가 되어서는 안 된다.

Ready 작업의 최초 실행을 예약할 때는 record 하나만 있어도 대기 worker를 깨울 수 있다.
Gate가 없는 owner를 추가 worker에 배정할 때는 아직 꺼내지 않은 record가 남아 있고,
application 실행에 사용할 수 있으며 아직 깨우기나 실행이 예약되지 않은 idle worker가
있어야 한다. 이 조건은 작업 게시와 worker가 idle이 되는 경계에서 확인한다. 이미
예약되었거나 실행 중인 worker에 같은 깨우기를 반복하지 않는다. 알림은 게시한 작업 묶음과
worker 가용 상태에 따라 합치며, 각 record의 enqueue나 take를 이유로 무조건 알리지 않는다.
수신 역할을 맡았거나 그 역할에 예약된 자원은 idle application worker로 세지 않는다.

이 판정은 기존 scheduler 상태 관리 안에서 끝내고, 알림은 그 상태 변경 구간을 벗어난 뒤
전달한다. Handler와 filter를 그 상태 변경 구간 안에서 호출하지 않는다. 별도 record별
notification queue나 dispatch task를 만들지 않는다.

내부 확인 조건 — record별 실행 소유권이 중복되지 않고, runnable record가 남은 gate 없는
owner의 ready 등록에 누락·중복이 없으며, enqueue와 마지막 take의 경쟁 및 ready 게시와
worker 대기 전환의 경쟁에서 작업이 누락되지 않는지 확인한다. 추가 worker 알림은 위의
가용 조건을 만족하고 이미 예약된 worker에 중복되지 않아야 한다. 이 조건들은 scheduling
상태 관리의 white-box 불변 조건이다.
```

**en**

```markdown
The set of ready owners represents the runnability of records admitted to owner queues with a
permit. The same owner is present at most once.

A Node or ChannelName owner without a gate remains in the set while runnable records not yet taken
by a worker remain. It is not excluded because another worker is already executing one of its
handlers. A take transfers one record at the queue head to exactly one worker and updates membership
according to the remaining records. The same scheduling state owner orders enqueue, removal on the
last take, and registration on a subsequent enqueue. Selecting the queue-head record does not grant
exclusive ownership of the owner. Runnability of owners with a gate follows the existing gate and
claim rules.

Before actually waiting, a worker registers so it can receive a notification and then rechecks the
ready set; it waits only if no work is available to it. Publication of ready state and transition to
waiting are coordinated so that either the publisher observes the waiting worker or the worker
observes the published work. A woken worker also rechecks the set. Loss of a transient notification,
or a race between publication and waiting, must not leave runnable work indefinitely unprocessed
with all available workers waiting.

Initial scheduling of ready work may wake a waiting worker even for a single record. Assigning an
additional worker to an owner without a gate requires records not yet taken and an idle resource
available for application execution with no wake or execution already scheduled for it. Check this
condition at work publication and at transitions to worker idleness. Do not repeat the same wake
for a worker already scheduled or running. Coalesce notifications according to the published batch
and worker availability, rather than notifying unconditionally on every record enqueue or take.
Resources holding or reserved for the receive role do not count as idle application workers.

Make these decisions within the existing scheduler state management and deliver notifications after
leaving its state-change span. Do not invoke handlers or filters within that span. Create no separate
per-record notification queue or dispatch task.

Internal confirmation conditions — each record has exactly one execution owner; registration of an
owner without a gate has no omission or duplicate while runnable records remain; races between
enqueue and the last take, and between ready publication and transition to waiting, lose no work.
Notifications for additional workers satisfy the availability conditions above and do not duplicate
an already scheduled wake. These are white-box invariants of scheduling state management.
```

### E4 — §04 atomic enqueue: refer notification to E3

Replace step 5 at `.ko.md:217–218` / `.en.md:243–244`. This keeps immediate scheduling intent without requiring a redundant notification for each empty/nonempty fluctuation while workers are active.

**ko**

```markdown
5. 비어 있던 queue가 채워졌으면 그 owner의 ready 상태를 게시하고, 이 절의 준비된 owner
   집합 규칙에 따라 즉시 실행 예약을 요청한다. 실제 알림은 commit 구간을 벗어난 뒤 전달한다.
```

**en**

```markdown
5. If the queue was empty, publish the owner's ready state and immediately request scheduling under
   this section's ready-owner rules. Deliver the actual notification after leaving the commit span.
```

### E5 — §04 permit lifetime: distinguish physical take from accounting departure

Replace step 5 at `.ko.md:120–123` / `.en.md:134–137`.

**ko**

```markdown
5. 각 job은 자신을 실행하는 callback handler의 실제 첫 instruction 직전까지 Application
   job queue의 permit을 보유하고 queued job으로 센다. Worker가 owner queue에서 record를
   먼저 꺼내더라도 이 계산과 permit 소유권은 바뀌지 않는다. Callback 전에 terminal로
   끝나면 그 정리 과정에서 permit을 정확히 한 번 반환한다. 그 밖에는 callback handler의
   실제 첫 instruction 직전에 반환한다. Node·Spot·Actor handler와 유입 종류에 따라 다른
   반환 시점을 두지 않는다.
```

**en**

```markdown
5. Each job retains its Application job queue permit and remains counted as a queued job until
   immediately before its callback handler's actual first instruction. A worker's earlier take from
   the owner queue changes neither this accounting nor permit ownership. A pre-callback terminal
   returns the permit exactly once during cleanup; otherwise it is returned immediately before the
   callback handler's actual first instruction. Node, Spot, and Actor handlers and different ingress
   kinds do not use different release points.
```

### E6 — budget unit and scheduler fairness

Append the following after the existing final paragraph of `01-execution/02-handler-turn-and-execution-gate` §9 (`.ko.md:405–406` / `.en.md:467–469`). The existing gated batch rule remains intact.

**ko**

```markdown
Gate가 없는 Node·ChannelName owner에서는 같은 예산을 worker별 연속 drain 구간에
적용한다. 구간은 그 worker가 해당 owner의 record를 처음 고를 때 시작하며, 같은 owner를
다시 고르거나 다른 worker가 record를 꺼낸다고 초기화하지 않는다. Worker가 다른 owner의
작업을 처리하거나 실제로 idle 상태가 되면 그 구간은 끝난다. W3가 동기로 완료되거나
suspend하여 worker가 W5로 돌아오는 경계에서 예산을 확인한다. Suspend한 handler의
terminal completion을 기다리면서 worker나 예산을 점유하지 않는다. 여러 worker의 시간을
owner별 합계로 더하지 않는다. 예산을 소진한 worker의 다음 owner 선택은
[Framework API §11](../00-foundation/06-framework-api.ko.md#11-handler-실행-객체와-dependency-수명)의
scheduler 공정성 규칙을 따른다.
```

**en**

```markdown
For a Node or ChannelName owner without a gate, apply the same budget to each worker's consecutive
drain interval. The interval starts when that worker first selects a record of the owner; selecting
the same owner again, or another worker taking a record, does not reset it. It ends when the worker
processes another owner's work or actually becomes idle. Check the budget when W3 completes
synchronously or suspends and the worker returns to W5. Waiting for a suspended handler's terminal
completion occupies neither the worker nor its budget. Do not sum several workers' time into an
owner-wide total. After budget exhaustion, selection of the worker's next owner follows the scheduler
fairness rule in [Framework API §11](../00-foundation/06-framework-api.en.md#11-handler-execution-object-and-dependency-lifetime).
```

Replace the occupation-bound paragraph in `00-foundation/06-framework-api.ko.md:547–550` / `.en.md:621–625`.

**ko**

```markdown
Gate가 있는 owner가 scheduler를 연속으로 점유하는 시간에는 상한이 있다. 상한에 도달하면
남은 작업을 ready 상태로 되돌리고 다른 ready owner에게 실행을 넘긴다. 이 상한은 같은
node의 다른 owner가 겪는 최대 대기 시간을 정한다. 실행 중인 handler 하나가 상한을 넘겨
실행되는 경우는 이 계약이 다루지 않으며, handler 경계에서만 확인한다.

Gate가 없는 Node·ChannelName owner에서는 한 worker가 같은 owner의 작업을 연속
drain하는 시간에
[Handler turn §9](../01-execution/02-handler-turn-and-execution-gate.ko.md#9-시간-예산과-batch-처리-구현)의
시간 예산을 적용한다. 예산을 소진하면 아직 꺼내지 않은 작업은 ready 상태로 유지하고,
그 worker는 대기 중인 다른 ready owner에게 실행 기회를 준 뒤 해당 owner의 drain을
계속할 수 있다. 다른 ready owner가 없으면 같은 owner를 계속 처리할 수 있다. 이미
대기 중인 owner를 반복해서 건너뛰어 실행 기회를 무기한 미루지 않는다. 이 규칙은 여러
worker가 같은 gate 없는 owner를 처리할 때도 worker마다 적용하며, 이미 실행 중인 handler를
중단하지 않는다. 같은 node의 다른 owner에 대한 고정 최대 대기 시간을 보장하지는 않는다.
```

**en**

```markdown
For an owner with a gate, there is a cap on how long it continuously occupies the scheduler. Once
the cap is reached, remaining work is returned to the ready state and execution is handed to another
ready owner. This cap sets the maximum wait time other owners on the same node experience. A single
running handler that runs past the cap is not covered; the cap is checked only at handler boundaries.

For a Node or ChannelName owner without a gate, each worker's consecutive drain of that owner is
subject to the time budget in
[Handler Turn §9](../01-execution/02-handler-turn-and-execution-gate.en.md#9-time-budget-and-batch-processing-implementation).
On exhaustion, records not yet taken remain ready, and that worker gives another waiting ready
owner an execution opportunity before continuing the expired owner's drain. If no other owner is
ready, it may continue the same owner. Repeated selection must not indefinitely postpone an owner
already waiting for execution. This gate-less rule applies independently to each worker even when several workers serve the same
owner; it does not interrupt handlers already running. It does not guarantee a fixed maximum wait
time for other owners on the node.
```

### E7 — §08 I4: correct the resource-switch counting unit

In the I4 table row (`.ko.md:187` / `.en.md:204`), replace **only the final cell**, preserving the rest of the row.

**ko**

```text
Framework가 만드는 실행 자원 전환은 각 record의 claim부터 해당 handler의 첫 instruction까지 최대 하나다(§4.4).
```

**en**

```text
From each record's claim to its handler's first instruction, the Framework introduces at most one execution-resource switch (§4.4).
```

### E8 — §08 verification: separate public behavior from internal scheduling evidence

Append the following to the existing internal-check paragraph at `.ko.md:288–290` / `.en.md:317–320`.

**ko**

```markdown
Gate가 없는 owner의 record take 순서와 ready membership 및 worker 대기 전환의 원자적
관계는 Application job queue §3의 내부 확인 조건으로 검증한다. 같은 owner를 여러 worker가
처리할 때도 그 조건을 적용한다. Worker별 drain 시간 예산이 같은 owner의 재선택으로
초기화되지 않고, I4부터 W3까지 record별 실행 자원 전환이 최대 한 번인지 확인한다.
```

**en**

```markdown
Verify record-take order for owners without a gate, and its atomic relationship to ready membership
and worker wait transitions, under the internal confirmation conditions of Application Job Queue §3.
Apply those conditions when several workers serve the same owner. Verify that reselecting the same
owner does not reset a worker's drain budget and that each record has at most one execution-resource
switch from I4 to W3.
```

Append the following item after (e) in §7, before the paragraph about failing (c) (`.ko.md:401` / `.en.md:444`).

**ko**

```markdown
- (f) **Gate가 없는 owner의 dispatch와 수명**: 여러 application 실행 자원을 제공하는
  runtime에서는 같은 Node·ChannelName owner에 보낸 독립 record들이 앞 handler의
  suspension이나 terminal completion을 기다리지 않고 실행될 수 있다. 실행 자원이 하나인
  runtime에서는 기존 비동기 suspension을 사이에 둔 독립 진행으로 확인한다. 두 경우 모두
  record가 중복 실행되지 않고, record별 handler·filter scope가 분리되며, 각 dispatch의
  terminal과 정리가 정확히 한 번 발생한다. 같은 gate를 사용하는 두 handler에는 기존
  직렬 실행 규칙을 확인한다. Gate가 없는 owner의 지속적인 부하 속에서도 다른 ready
  owner가 worker별 시간 예산 규칙에 따라 실행 기회를 얻는다. Shutdown은 이미 worker가
  꺼냈거나 실행 중인 record도 accepted work로 처리한다. 검증 방법은 언어별 문서가 연결한다.
```

**en**

```markdown
- (f) **Dispatch and lifetime for owners without a gate**: in runtimes providing several application
  execution resources, independent records of the same Node or ChannelName owner can execute
  without waiting for the preceding handler's suspension or terminal completion. A runtime with
  one execution resource verifies independent progress across existing asynchronous suspension.
  In both cases, records are not executed twice, handler and filter scopes are separate per record,
  and each dispatch reaches its terminal and cleanup exactly once. Verify the existing serial rule
  for two handlers sharing a gate. Under sustained load for an owner without a gate, other ready
  owners receive execution opportunities under the per-worker time-budget rule. Shutdown treats
  records already taken by workers or running as accepted work. The language pages link the
  verification methods.
```

## Implementation acceptance examples

These are acceptance scenarios for the revised spec, not requests to build or benchmark during this review.

- Pause worker A **after take, before pipeline entry**, allow worker B to enter, then resume A. Record takes must remain A, B. E1 allows pipeline entry B, A; the old §02 does not. This scenario makes the contract choice directly testable.
- For C++, .NET, and Java with two available application resources, hold A in a synchronous callback using an externally released test barrier and observe independent B on the same gate-less owner. Repeat with a shared gated owner as a negative control; B must wait. On Node, use an async barrier, and separately verify that synchronous code remains on its single event loop.
- Publish ready work immediately before wait registration, during the register/recheck boundary, and after the worker starts waiting. A lone record must run without another enqueue, polling tick, or timeout. Repeat with a nonempty gate-less queue whose other worker is occupied. Instrument redundant notifications separately; no correctness test should require exactly one wake per batch regardless of available workers.
- Race the last take against enqueue; leave one record ready. The owner must be registered once and the record executed once. Check pre-start permit accounting while a taken record is paused before the callback. Intentionally considering dequeue as permit return must fail this accounting check.
- Keep one gate-less owner continuously ready across several workers, then make another owner ready. Check worker budgets at W5, including same-owner reselection and suspended handlers, and confirm another owner's execution opportunity. A deliberately reset-on-every-take implementation must fail the budget check.
- Use distinct per-dispatch scope markers, a suspending filter, a rejecting filter, decode failure, and mixed completion order. Observe scope isolation and exactly-once terminal cleanup. Exercise drain with the queue empty but two dispatches still outstanding; teardown must not treat the ready set as an in-flight registry.
- Send A and B requests on one ClientServer connection; hold A and complete B. B's correlation must complete B alone, followed by A or A's existing terminal outcome. If traffic precedes B's submitted reply on that connection, the existing Core FIFO still applies. No request-order reply assumption is valid.

Rule ownership after the proposed revision: application ordering remains in §02; ready membership and wake decisions remain in §04; per-worker drain interval is defined in §02 §9; inter-owner selection fairness remains in Foundation §11. Decision-owner counts are **1 → 1 for each of these decisions**; the change removes conflicting interpretations rather than adding a second scheduling authority. No new state, queue, lock, timer, helper, or public API is proposed by this review. The rejected alternatives are an implicit new pipeline-start sequencer and a global aggregate owner-budget authority.
