# Spec review — gate-less owner multi-worker drain (0.33.0 #1639)

Reviewer: Fable (independent of sol-review.md, not read). Worktree read-only: /root/worktree/zlink-3868t.
Change: /root/jobs/fx/cx/v21/s033/specrev/spec.diff (01-execution/02, 04, 08; ko+en).
All line numbers below are from the worktree with the diff applied. `S/` = framework/doc/framework/common/spec/server/.

**Verdict: approve with the listed edits (A–D). No clause in the tree contradicts the change; the gaps are precision gaps that four runtimes would fill differently.**

---

## Q1. Conflicts with other clauses

Checked: 00-foundation (glossary, 04-interaction-model, 05-message-model, 06-framework-api), 01-execution 01–08, 02-channel-transport/02,03, 03-cancellation, 05-location-relocation, 04-session, 06-observability, languages/*.

| Clause | Finding |
|---|---|
| `S/01-execution/02-handler-turn-and-execution-gate.en.md:102-109` (Node/ChannelName handlers have no gate; start in order received; next record does not wait; same handler runs concurrently for several records; application owns synchronization) | **Compatible.** This is the clause that already licenses the change. No edit needed beyond Edit C. |
| `S/01-execution/02-…en.md:110-112` (two turns on the same gate never run concurrently) | Compatible: gate-less owners have no gate. |
| `S/01-execution/02-…en.md:291-301` §6 "Only one processing authority per owner with a gate exists at a time" | Compatible: scoped to owners with a gate. |
| `S/01-execution/02-…en.md:356-434` §7 lanes, `:454-469` §9 time budget ("the same value applies per execution object") | **Imprecise after the change** — with N workers on one gate-less owner, "per execution object" no longer says whether the 10 ms is per worker or summed. Covered by Edit D (placed in Framework API §11, which §7 of 07 names as the owner of inter-owner order) and by the W5 sentence in Edit A. |
| `S/01-execution/04-application-job-queue-and-backpressure.en.md:213-219` ready set (as edited) | **Imprecise**: "may stay in the set while a worker is taking one of its records, if records remain" does not say who removes the owner, or that removal and the enqueue of step 5 (`:242-244`) are one atomic decision. Without that, the take-last/enqueue race loses the owner (see Q3). Edit B. |
| `S/01-execution/04-…en.md:221-223` internal condition "a woken execution resource always rechecks" | Needs "and before it waits" for the wake rule to be lost-wakeup free. Edit B. |
| `S/01-execution/04-…en.md:242-244` step 5 "If the queue was empty, put that owner into the set" | Compatible once Edit B makes "leave the set" part of the same atomic section. |
| `S/01-execution/06-state-ownership-and-lanes.en.md:180-181` "Only one turn executes at a time" | Scoped to state lanes (component state), not to Node/ChannelName owner queues. No conflict. |
| `S/01-execution/07-serial-executor-layers.en.md:309-325` §6.4/§6.5 (one drain loop per owner, "if it is already running, nobody wakes it") | Scoped to serial execution objects (Spot/Actor/Session, `:37`, `:359-362` of 02). Gate-less owners are not serial executors. No conflict, but note the drain-flag pattern of §6.5 is exactly what a multi-worker gate-less owner must **not** use; Edit A says so implicitly by making the set-membership decision atomic with the take. |
| `S/01-execution/08-messaging-hot-path.en.md:243` W1 "Takes an owner from the ready set" | **Conflicts in wording** with the new paragraph: for a gate-less owner the worker takes a *record* and must not remove the owner. Edit A rewrites the paragraph to say W1 takes a record, not the owner. |
| `S/01-execution/08-…en.md:247` W5 "moves to the next owner" | For a gate-less owner that stays in the set, "next owner" must be a *different* ready owner, else the budget is void. Edit A. |
| `S/01-execution/08-…en.md:269-272` "This page does not reduce handler concurrency" | Compatible. |
| `S/01-execution/08-…en.md:317-318` internal check "ready-owner registration has no omission or duplicate" | Still holds under Edit B (exactly once while non-empty). |
| `S/01-execution/08-…en.md:349` §4.4 "Independence between completion callbacks … does not mean a new application callback runs while every application execution resource is held by a synchronous wait" | Compatible; unchanged. |
| `S/01-execution/08-…en.md:399-402, 409` §6 Node: single event loop, "side by side" = alternation between awaits | Compatible, but the new paragraph should say Node is unaffected (Edit A). |
| `S/00-foundation/06-framework-api.en.md:613-614, 621-625` scheduler drains "a ready owner's mailbox", occupancy cap "sets the maximum wait time other owners … experience" | **Needs a sentence** for N workers on one owner (Q4, Edit D). |
| `S/00-foundation/06-framework-api.en.md:552-556` §10 scope per dispatch; `:577` §11 "Channel handler and filter: from dispatch start to terminal completion" | Compatible; nothing per owner. |
| `S/00-foundation/02-glossary.en.md:959-989` Execution Gate / Handler Turn | Compatible; boundaries delegated to 02 §2–§3, which Edit C keeps precise. |
| `S/00-foundation/04-interaction-model.en.md:250-252, 637-638` "Messages submitted by the same origin to the same destination pipe are FIFO / arrive in FIFO order" | Delivery/arrival order, not processing order. Compatible (see PM-1). |
| `S/02-channel-transport/03-client-server-channel.en.md:600-607` single FIFO | DATA vs reply on the Core connection; nothing about reply order among requests. Compatible. |
| `S/01-execution/03-cancellation-and-shutdown.en.md:121-160` drain waits for started work | Unchanged: started handlers of a gate-less owner are already N-in-flight (async overlap). |
| 05-location-relocation, 04-session | Relocation moves Spot/Actor/Session queues; Node/ChannelName records are not relocated; Session is a serial execution object with a gate (02 §7 `:359`). No conflict. |

## Q2. Observable change to applications

What a gate-less handler could already see per `02:102-109`: another record's filter/handler pipeline entering before its own terminal completion, undefined handler-body entry order after filters, undefined completion order, and (from `08:121-122`) only *submit* order FIFO on the source side.

| Surface | Before (de facto, thread runtimes) | After | Spec-visible change? |
|---|---|---|---|
| Synchronous (never-suspending) handler, one owner | serialized by the single draining worker | may run on two threads at once | **No spec change** (`02:108-109` already says the same handler runs concurrently and the application owns synchronization), but this is the one *implementation-visible* change. Edit C spells out "on different execution resources" so nobody reads "concurrently" as await-interleaving only. |
| Filters (`06-framework-api:534-556`) | per-dispatch, may already interleave across awaits (`02:106-108`) | may run truly parallel for one owner | No; same clause. |
| DI scope (`06-framework-api:552-556`, `08:274-278`) | one scope per dispatch | unchanged | No. |
| Metrics (06-observability/02-runtime-metrics) | no per-owner concurrency metric exists | — | No. |
| Reply order to one caller on one connection | already undefined for async handlers; in order for sync handlers de facto | undefined | No spec promise exists (`04-interaction-model:251` is about arrival; `03-client-server-channel:600-607` is DATA-vs-reply). Each reply is matched by correlation (`03-client-server-channel` §5). |
| ClientServer reply vs earlier one-way DATA on the same DEALER–ROUTER connection | Core FIFO | unchanged (W4 still runs in the handler's turn) | No. |
| Spot/Actor/Session, timers, lifecycle | gated | unchanged | No. |
| Node runtime | single loop | unchanged | No. |

Conclusion: nothing observable beyond the existing gate-less contract; only sync handlers on thread-based runtimes lose an accidental serialization that the spec never granted.

## Q3. Precision and the wake rule

Problems in the current wording (`08:252-258`, `04:213-219`):

1. **Who removes the owner, and when.** "stays in the set … if records remain" leaves the removal implicit. Race: worker A pops the last record (queue empty) → ingress enqueues r2, sees "queue was empty", adds O (already present, so no-op) and notifies → A, acting on its stale "remaining == 0", removes O. O has r2 and is in no set; nobody is woken for it. The 04 recheck invariant does not save this because the owner is not *in* the state being rechecked. Fix: the leave/stay decision is made in the **same atomic section** as the take, under the same rule as step 5 of "Do Not Separate…" (`04:242-244`). Edit A + B.
2. **W1 "takes an owner from the ready set" (`08:243`)** reads as *removing* it. For gate-less owners the worker must take a record and leave the set entry alone. Edit A.
3. **Two workers see O; one finds the queue empty.** Must be stated as normal (return to W1), otherwise an implementer may treat it as the "omission/duplicate" violation of `08:317`. Edit A.
4. **"an idle worker exists"** has no meaning in .NET (ThreadPool work items, `08:407`) or Java (lane workers). Phrase it against the worker limit: number of drain work items running or scheduled below the limit. Edit A.
5. **"never once per record"** is a cost rule, not a correctness rule; the text should say correctness rests on the recheck invariant and bound the extra wake to **at most one** per take (no thundering herd). Edit A.
6. **Lost-wakeup freedom.** With Edit B ("rechecks after waking and before waiting") and the owner staying in the set while non-empty, the rule is sound: either some worker is running (it rechecks before waiting and will find O), or all are waiting, in which case the last take that left records remaining saw idle workers and woke one, or the enqueue path of step 5 notified. The only hole was (1).
7. **Node** should be excluded explicitly (one application execution resource, `08:400-401, 409`). Edit A.

With Edits A and B the same shape falls out in all four runtimes: C++ (worker threads take from a shared head; set membership flips in the queue's atomic section), .NET (one drain work item per running worker, ≤ limit, each item loops over the ready set), Java (lane workers, same), Node (unchanged).

## Q4. Scheduler occupation bound (Framework API §11, en `:621-625`, ko `:547-550`)

Today the cap bounds how long one owner holds *the* worker, and "sets the maximum wait time other owners on the same node experience". With N workers on a gate-less owner, each worker individually still leaves at the cap, so the bound on another owner's wait is kept (any worker reaching its cap takes a different owner). What changes is the owner's *total* share: up to N × cap. The text must say the cap is per worker-run of that owner, and W5's "next owner" must be a different ready owner. Edit D (+ the W5 sentence in Edit A). `02:467-469` §9 ("the same value applies per execution object") stays true read as "per worker-run of that object".

## Q5. Edits (exact replacement text)

Anchors used below follow the existing heading-to-anchor pattern in these files; run the doc link checker after applying.

### Edit A — `S/01-execution/08-messaging-hot-path.en.md:252-258` (whole bold paragraph) and `…ko.md:229-236`

**en — replace with:**

```
**Several workers may start the records of an owner without a gate.** For such an owner, W1 takes one
record from the owner-queue head, not the owner; after the take the worker holds nothing of that owner.
The take fixes the start order. Whether the owner stays in the ready set is decided in the same atomic
section as the take, by the rule of [Application Job Queue "Do Not Separate the Admission Decision from Enqueueing"](04-application-job-queue-and-backpressure.en.md#do-not-separate-the-admission-decision-from-enqueueing-implementation):
it leaves the set only when the take finds the queue empty; otherwise it stays, present once. A take
that finds the queue empty because another worker took the last record is not an error; the worker
returns to W1. A single owner's handlers therefore start in FIFO order, but started handlers may run
side by side on different workers, and their completion order is not defined. After a take that leaves
records behind, if an application worker is idle — in a task-based runtime, if the number of drain work
items running or scheduled is below the worker limit — the worker wakes at most one more worker through
the ready-set notification of I4. It never wakes once per record, and correctness does not rest on this
wake: a worker rechecks the ready set after it is woken and before it waits
([Application Job Queue "The Set of Ready Owners"](04-application-job-queue-and-backpressure.en.md#the-set-of-ready-owners-implementation)).
At W5 the time budget bounds one worker's continuous run of that owner; when it is exhausted the worker
takes a different ready owner if one exists. A runtime with one application execution resource (Node)
has no second worker, so this paragraph changes nothing there. An owner with a gate is processed by the
one worker holding the gate, through W1–W5 as before.
```

**ko — replace with:**

```
**Gate가 없는 owner의 record는 여러 worker가 나눠 시작할 수 있다.** 이런 owner에 대해 W1은 owner가 아니라
owner queue head의 record 하나를 꺼내며, 꺼낸 뒤 worker는 그 owner에 대해 아무것도 보유하지 않는다. 꺼내는
동작이 시작 순서를 정한다. Owner가 준비된 owner 집합에 남는지는 꺼내는 동작과 같은 원자 구간에서
[Application job queue 「넣을지 판단하는 것과 넣는 것을 쪼개지 않는다」](04-application-job-queue-and-backpressure.ko.md#넣을지-판단하는-것과-넣는-것을-쪼개지-않는다-구현)의
규칙으로 정한다. 꺼낼 때 queue가 비어 있음을 확인한 경우에만 집합에서 빠지고, 그렇지 않으면 한 번만 들어 있는
채로 남는다. 다른 worker가 마지막 record를 먼저 꺼내 queue가 비어 있으면 오류가 아니며, worker는 W1로 돌아간다.
그래서 한 owner의 handler 시작 순서는 FIFO이지만, 시작한 handler들은 서로 다른 worker에서 나란히 실행될 수 있고
종료 순서는 정하지 않는다. 꺼낸 뒤 record가 남아 있고 쉬는 application worker가 있으면 — task 기반 runtime에서는
실행 중이거나 예약된 drain work item 수가 worker 상한보다 적으면 — I4의 준비된 owner 집합 알림으로 worker를 최대
하나 더 깨운다. record마다 깨우지 않으며, 정확성은 이 깨우기에 의존하지 않는다. worker는 깨어난 뒤와 대기에
들어가기 전에 준비된 owner 집합을 다시 확인한다([Application job queue 「준비된 owner 집합」](04-application-job-queue-and-backpressure.ko.md#준비된-owner-집합-구현)).
W5의 시간 예산은 worker 하나가 그 owner를 연속 처리하는 시간을 제한하며, 예산을 다 쓴 worker는 다른 준비된 owner가
있으면 그 owner를 잡는다. Application 실행 자원이 하나인 runtime(Node)에는 두 번째 worker가 없으므로 이 단락은
아무것도 바꾸지 않는다. Gate가 있는 owner는 W1~W5 그대로 gate를 가진 worker 하나가 처리한다.
```

### Edit B — `S/01-execution/04-application-job-queue-and-backpressure.en.md:213-223` and `…ko.md:191-201`

**en — replace the bullet (`:213-219`) with:**

```
- **Maintain as state the set of owners that currently have work to do.** This state represents
  "this is the current state," not a one-time "something changed" notification, and the
  same owner never enters it twice. An owner without a gate, whose records several workers may
  take ([Messaging Hot Path §4.3](08-messaging-hot-path.en.md#43-the-handler-execution-unit)),
  stays in the set while its queue holds a record and leaves it only when a take finds the queue
  empty; that decision is made in the same atomic section as the take and as the enqueue of step 5
  below, so whenever its queue is non-empty it is in the set exactly once. Because an owner with
  remaining work must eventually be processed even if a notification is lost, an execution
  resource rechecks this state after it is woken and before it waits.
```

**en — replace the internal condition (`:221-223`) with:**

```
Internal confirmation condition — that an execution resource rechecks the set of ready owners after
waking and before waiting, so that no wakeup is missed even when a notification is lost, is a
white-box invariant of this state management.
```

**ko — replace the bullet (`:191-197`) with:**

```
- **지금 처리할 일이 있는 owner를 상태로 유지한다.** 이 상태는 "무엇이 바뀌었다"는 1회성
  알림이 아니라 "지금 이 상태다"를 나타내며, 같은 owner가 중복해서 들어가지 않는다. 여러 worker가
  record를 꺼낼 수 있는 gate 없는 owner([Messaging hot path §4.3](08-messaging-hot-path.ko.md#43-handler-실행-단위))는
  queue에 record가 있는 동안 집합에 남고, 꺼낼 때 queue가 비어 있음을 확인한 경우에만 빠진다. 이 판단은
  꺼내는 동작, 그리고 아래 5단계의 enqueue와 같은 원자 구간에서 하므로, queue가 비어 있지 않은 owner는
  언제나 집합에 한 번만 들어 있다. 알림이 유실돼도 처리할 일이 남은 owner는 결국 처리돼야 하므로,
  실행 자원은 깨어난 뒤와 대기에 들어가기 전에 이 상태를 다시 확인한다.
```

**ko — replace the internal condition (`:199-200`) with:**

```
내부 확인 조건 — 실행 자원이 깨어난 뒤와 대기 전에 준비된 owner 집합을 다시 확인해 알림이 유실되어도 missed
wakeup이 없다는 것은 이 상태 관리의 white-box 불변 조건이다.
```

### Edit C — `S/01-execution/02-handler-turn-and-execution-gate.en.md:84-86` and `…ko.md:80-82`

**en — replace:**
```
handler start through terminal completion, and may run side by side with other turns of the same
owner — on different execution resources where the runtime has several
([Messaging Hot Path §4.3](08-messaging-hot-path.en.md#43-the-handler-execution-unit)).
```

**ko — replace:**
```
completion까지이며, 같은 owner의 다른 turn과 나란히 — 실행 자원이 여럿인 runtime에서는 서로 다른 실행
자원에서 — 실행될 수 있다([Messaging hot path §4.3](08-messaging-hot-path.ko.md#43-handler-실행-단위)).
```

### Edit D — `S/00-foundation/06-framework-api.en.md:621-625` and `…ko.md:547-550` (append to the occupancy-cap paragraph)

**en — append after "it's only checked at handler boundaries.":**
```
For an owner without a gate whose records several workers take
([Messaging Hot Path §4.3](../01-execution/08-messaging-hot-path.en.md#43-the-handler-execution-unit)),
the cap applies separately to each worker's continuous run of that owner; the owner's total share
of the scheduler may reach the number of workers times the cap, and the bound on other owners'
wait is kept because each worker leaves that owner at its own cap and takes a different ready owner.
```

**ko — append after "handler 경계에서만 확인한다.":**
```
여러 worker가 record를 꺼내는 gate 없는 owner([Messaging hot path §4.3](../01-execution/08-messaging-hot-path.ko.md#43-handler-실행-단위))에는
이 상한을 worker마다 그 owner를 연속 처리하는 시간에 각각 적용한다. 그 owner의 scheduler 점유 합계는 worker 수 ×
상한까지 커질 수 있고, 다른 owner의 대기 상한은 각 worker가 자기 상한에서 그 owner를 떠나 다른 준비된 owner를
잡으므로 유지된다.
```

---

## PM-1. Does any clause promise per-sender *processing* order for Node/ChannelName handlers?

No. Every ordering clause found is about submission, arrival/queue acceptance, or handler *start*:

| Clause | What it orders |
|---|---|
| `S/00-foundation/04-interaction-model.en.md:250-252` "Messages successfully submitted by the same origin to the same destination pipe are FIFO" | delivery to the destination pipe |
| `S/00-foundation/04-interaction-model.en.md:637-638` "…arrive in FIFO order" | arrival |
| `S/01-execution/08-messaging-hot-path.en.md:121-122` "Owner turns run FIFO, so a caller that submits 100 requests back to back gets all 100 into the binding in submit order" | source-side submit order |
| `S/01-execution/04-application-job-queue-and-backpressure.en.md:127-135, 242` permit order "oldest-live-source", "Commit the accepted-order sequence and append the message to the owner queue" | queue acceptance order |
| `S/01-execution/02-handler-turn-and-execution-gate.en.md:104-109` (ko `:100-105`) "Records start their handlers in the order received, and the next record does not wait for the previous handler to complete … entry order after filters is not defined … same handler runs concurrently" | **start** order only; explicitly disclaims the rest |
| `S/01-execution/08-messaging-hot-path.en.md:249-250` (ko `:226-227`) "starts handlers in owner-queue FIFO order, and the next record does not wait" | start order only |
| `S/02-channel-transport/03-client-server-channel.en.md:600-607` "Request Completion and the Single FIFO" | DATA vs reply on one Core connection |
| `S/02-channel-transport/06-wire-protocol.en.md:806` "Only order accepted into the target queue is preserved" | relocation relay acceptance |
| `S/00-foundation/05-message-model.en.md:91-94` `receivedAt` | timestamp, not order |
| framework/doc/framework/node/guide/server/33-backpressure.en.md:158-160 "a handler never infers ordering from arrival order" | guide: disclaims |
| framework/doc/framework/common/guide/server/32-execution-model.en.md:33-40, 99; 21-spot.en.md:183; 01-overview.en.md:165; index.en.md:422 | "processed in order / one at a time" promises are made **for Spots/Actors only** |

Guides 20-channel-messaging, 30-channel-patterns, 31-handler-dispatch and the language reference `03-messaging-execution` pages (cpp, dotnet, java, node, kotlin) make no processing-order statement for Channel/Node handlers. ko mirrors match (checked 02 ko `:100-105`, 08 ko `:226-227`).

## PM-2. User code assuming serial execution of a Node/ChannelName handler

Searched framework/templates, framework/languages/shared_sample, framework/languages/{cpp,dotnet,java,node}/samples|templates, framework/languages/java/zlink-framework-doc-examples for `IZLinkRequestHandler/IZLinkSendHandler/IZLinkRouteRequestHandler/IZLinkRouteSendHandler`, `ZLink(Route)?(Request|Send)Handler`, and C++ `request_type/reply_type` handler classes (38 handler files + 20 C++ host files). Every shared dependency mutated from such a handler is already synchronized or immutable; **no file assumes serial execution**:

| File | Shared state reached from the handler | Protection |
|---|---|---|
| framework/languages/dotnet/samples/Bingo/Server/Api/BingoPlayerRecordStore.cs:8 (used by Api/Handlers/GetPlayerRecordHandler.cs:9,20 and ReportBingoResultHandler.cs:9,20) | `_records` | `ConcurrentDictionary` |
| framework/languages/java/samples/java/Bingo/Server/Api/src/main/java/systems/zlink/samples/bingo/server/api/BingoPlayerRecordStore.java:9 | `records` | `ConcurrentHashMap` |
| framework/languages/node/samples/Bingo.Ts/Server/Api/Handlers/player-record-handlers.ts:15-27 | plain `Map`, read-modify-write in `report()` | safe: Node has one application execution resource (`08:400-401, 409`) and there is no `await` between the read (`:22`) and the `set` (`:26`); unchanged by this spec |
| framework/languages/dotnet/samples/ZoneWorld/Server/Ops/Application/Ops/NodeRegistry.cs:22-26 (from Infrastructure/ZLink/Handlers/OpsReportHandlers.cs:47) | `_nodes`, `_nodeByRoutingId`, `_routingIdByNode` | `ConcurrentDictionary` |
| framework/languages/dotnet/samples/ZoneWorld/Server/Ops/Infrastructure/ZLink/Sessions/OpsConsoleRegistry.cs:16,22,43,61,70,89 (from OpsReportHandlers.cs:38) | `_consoles`, `_recentAlerts` | `ConcurrentDictionary`, `lock` |
| framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/configuration/NodeRegistry.java:18-20,33,42,68 | maps | `ConcurrentHashMap` + `synchronized` |
| framework/languages/java/samples/java/ZoneWorld/Server/src/main/java/systems/zlink/samples/zoneworld/server/ops/OpsConsoleRegistry.java:12-13 | lists | `CopyOnWriteArrayList` |
| framework/languages/cpp/samples/GameQuest/Server/GameApi/main.cpp:32-139 `game_api_store_t` | `_bindings`, `_projections`, `_events`, `_unpublished_kills` | every method takes `std::lock_guard (_mutex)` |
| framework/languages/cpp/samples/DeliveryDispatch/Server/Dispatch/main.cpp:103-127 | `_offers`, `_pending_*` | `std::mutex _mutex` |
| framework/languages/cpp/samples/ZoneWorld/Server/Ops/main.cpp:35-86 | `_streams` | `std::mutex _mutex` |
| framework/languages/cpp/samples/ShoppingMall/Server/CommerceApi/main.cpp:140-200 | JSON state via `_store.update(...)`/`_store.read(...)` closures | store API serializes access |
| framework/languages/cpp/samples/SupportChat/Server/Api/main.cpp:38-58 `user_directory_t` | `_users` | read-only (`find` is `const`, map initialized once) |

Static members found in handler files are all pure helper methods or constants (e.g. dotnet SupportChat/Server/Api/Handlers/AuthenticateUserHandler.cs:36, java …/tracking/handlers/DeliveryStatusChangedHandler.java:17). Guide chapters 20/30/31/32 contain no channel-handler snippet with mutable shared state. Templates contain no Channel/Node handler.

## PM-3. Overlap with spec PR #3889

Read `gh pr diff 3889` (780 lines; files: glossary en/ko, 02-channel-transport/06-wire-protocol, 03-spot-actor/03,08,10, 05-location-relocation/01,02,06). **No semantic overlap** with this change:

- Glossary hunks are at en `:2195-2240` (Owner lease / `ZLinkOwnerLeaseReadResult`), not the Execution Gate / Handler Turn entries (`:959-989`) this change leans on.
- 10-spot-timer and 01-location-runtime §5 (diff `:276-290`, `:355-372`) tie Spot/Actor/timer *execution rights during a Store outage* to "the existing execution gate" — gated owners only. The sentence "The same object must not execute at two owners concurrently" (diff `:362`) uses "owner" in the MeshNode/Location-Store sense, which `02:359-362` already distinguishes from the execution-object "owner" used in 08 §4.3; no cross-reference is needed.
- 06-wire-protocol §5.1 "Owner Liveness Check" and 08-routing / 02-location-store-redis / 06-failure-failover hunks are Store/lease/routing text; nothing about worker drains, ready set, or Node/ChannelName handlers.
- No textual collision: #3889 does not touch 01-execution/02, 04, 08 or 00-foundation/06-framework-api.
