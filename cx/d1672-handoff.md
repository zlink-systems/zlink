# d1672 — §4.4 receive-role hand-off design (C++ RouteMesh, server I4 and client E5)

## 0. Summary

- **Where the 2 switches come from.**
  - Server I4: the per-node `std::thread` receive loop ends its turn, and `end_application_receive_turn` posts each ready owner to the `zlink-mesh-app` `offload_executor_t`.
  - Client E5: the reply completion is drained on the client's receive thread inside `port->poll`, then posted to the one process-wide completion-dispatcher `_worker`.
- **Recommendation: Design A.** Make the existing host executor `_application_dispatch` the only placement owner. Turn the receive loop into a "receive role" item that this executor's workers take ahead of application work. Remove `_threads`.
  - At the end of each loop iteration, the holder asks the executor, under its `_mutex`, to give the role to an idle worker. If that succeeds, the holder runs one unit of work itself: the first ready owner turn, or the first completion collected during its rotation. Everything else stays on the existing paths.
  - Expected switches: request-serial 0+0 per round trip; send-saturation server about 0, never more than one per owner turn.
- **Two decisions are needed from you before implementation (§4.3).**
  1. How to read the "completion callback independence" row when one rotation collects k > 1 completions (request-window, and sends under HWM backpressure).
  2. What happens when an exception escapes a rotation.

## 1. Current structure (file:line)

### 1.1 Receive (ingress owner)

- **One OS thread per mesh node.** Created in the loop over `_nodes` at `mesh/mesh_node_host_service.cpp:2523–2567` (`_threads.emplace_back`). Stored at `mesh/mesh_node_host_service.hpp:194`, joined in `stop()` at `:2991–2994`.
- **State that lives on that thread's stack and belongs to the receive role:**
  - `receive_permit`: a `shared_ptr<optional<permit_t>>` made at `:2526`. It is captured by the mailbox prepare lambda at `:2530–2554`, which moves it into each queued record (`mark_queued` at `:2548`, released at handler entry at `:2549–2551`).
  - `supply`: an `application_supply_slot_t` at `:2568` (`dispatch/application_job_queue.hpp:1086–1205`). Its wake is `signal_dispatch_activity`, which does not depend on any particular thread.
  - `pending_management`: at `:2572–2596`.
  - Per-rotation `application_permit` and `application_permit_budget`: `:2599–2613`. The budget is returned at `:2756–2757`.
  - Closed at `:2800` (`supply.close()`).
- **Rotation steps:**
  - `begin_application_receive_turn` at `:2615`.
  - `node->dispatch_ready(...)` at `:2620`, which goes through `mesh/mesh_node_runtime.cpp:4370–4380` to `stateful/public_host_runtime.cpp:6122–6359`. Management (relays, liveness tick, `expire_requests` at `:6136`) runs once per turn. There is one readiness poll per turn (`begin_receive_turn` at `:6156`), then `receive_next` until no data or the budget (`:6161–6356`).
  - `end_application_receive_turn` at `:2754`.
  - I0 wait only when `count == 0`: `wait_for_dispatch_activity` at `:2794`, which goes to `stateful/public_host_runtime.cpp:6548–6568`, then `mesh/raw_mesh_node_owner.cpp:3442–3483`, then `port->poll(timeout)`.
- **Socket, poller and lifecycle locks:**
  - `raw_route_port_t::poll` at `backend/raw_route_port.cpp:346–393` holds `_poller_mutex` (`:349`) around `_poller->wait`.
  - `receive_if_ready` at `:400–429` holds `*_socket_mutex`.
  - `close` at `:460–472` holds both.
  - The raw owner's `_port`, `_ingress_poller` and `_router` are created and closed at `mesh/raw_mesh_node_owner.cpp:512–566, 629–643`. Snapshots are taken under `_lifecycle_mutex` (`:2670–2674`, `:3467–3470`).
  - The binding has no thread-affinity check. A grep of `bindings/cpp/src/Runtime/Eventing/` finds no `this_thread::get_id` or owner-thread field. Polls only have to be serialized, and `_poller_mutex` already does that.
- **Receive rotation boundary.** End of loop iteration: after `dispatch_ready` returned (`:2753`), `end_application_receive_turn` (`:2754`), permits returned (`:2756–2757`), and the optional I0 wait returned (`:2794–2797`), before the next `while` check at `:2597`. At that point no `_poller_mutex`, socket lock, lane scope or binding drain stack is held.

### 1.2 Ready-owner placement (I4)

- **During the turn,** `try_enqueue` (`mesh/service_mailbox.cpp:42–75`) marks the owner ready (`:66–69`). It does not notify, because `_application_receive_turn` is set (`:70`, `:263–267`).
- **At the boundary,** `end_application_receive_turn` (`:269–276`) calls `notify_application_ready` (`:278–289`). That swaps out the whole ready deque and calls the bound `_application_ready` callback once per owner.
- **That callback** (`mesh/mesh_node_host_service.cpp:2555–2565`) calls `_application_dispatch->try_submit_internal(drain_application_owner(...))`. If that returns false, it runs the reject-only drain inline (`:2563`).
- **The same notify also fires from other threads** outside a receive turn: worker `release` (`:185–216`), `end_application_drain` (`:242–261`), and caller-thread `try_enqueue`, e.g. `submit_local_node_send` at `mesh/mesh_node_host_service.cpp:3184`.
- **`drain_application_owner`** (`:3101–3136`):
  - `begin_application_drain` flips the owner phase ready → draining (`service_mailbox.cpp:232–240`); this flip is the claim.
  - It then loops `dispatch_application_owner` (`stateful/public_host_runtime.cpp:6510–6546`), which takes ≤64 records per claim (`try_claim_owner` at `service_mailbox.cpp:98–119`).
  - The time budget is `owner_time_budget` = 10 ms (`dispatch/dispatch_limits.hpp:15`).

### 1.3 Application executor

- **Pool:** `offload_executor_t(min 1, max max(2, hw), idle 100 ms, "zlink-mesh-app")`, one per host service, shared by all its nodes (`mesh/mesh_node_host_service.cpp:683–687`).
- **Idle tracking:** `_idle_workers` is incremented and decremented around `_ready.wait` under `_mutex` (`dispatch/offload_executor.cpp:224–234`). Workers above the minimum retire after the idle timeout (`:235–241`).
- **Assignment is not exclusive.**
  - `try_submit_internal` (`:62–84`) appends to one FIFO `_queue`, and whichever worker takes `_mutex` first pops the front (`:248–249`).
  - It starts a new worker if `_idle_workers == 0 && _live_workers < max` (`:78–79`).
- **Shutdown:**
  - `drain_until` sets `_stopping`, waits for `_queue.empty() && _active == 0`, then waits for `_live_workers == 0` and joins (`:124–158`).
  - `try_submit_internal` keeps accepting while `_active != 0 || !_queue.empty()` (`:69`).
- **Exceptions from work items are swallowed** (`:255–264`). A `std::thread` receive loop that throws today calls `std::terminate`.

### 1.4 Completion dispatcher (E5)

- **One process-wide `operation_completion_dispatcher_t`** (`foundation/operation_registry.cpp:102–242`), created as a static at `:247–253`. It has one `std::thread _worker` (`:107`, `:241`).
- **Posting** is a lock-free intrusive stack: `post` / `post_chain` at `:134–166`. Only the empty → non-empty transition calls `notify_one` (`:159–160`).
- **The worker** takes the whole stack, reverses it, and runs callbacks one by one. Each callback's exceptions are isolated and its drain slot released (`:212–237`). It sets thread_local `completion_dispatcher_current` (`:95–100`, `:214`).
- **Drain slots:** the item constructor admits one; release happens on callback return or destruction (`:16–69`). `shutdown` waits on them unless it is called on the dispatcher itself (`:469–492`).
- **Why that flag matters for continuations.** The host continuation scheduler resumes a caller's coroutine inline only on a completion-dispatcher or coroutine-executor thread. Anywhere else it posts to `coroutine_executor_t` (`dispatch/coroutine_executor.cpp:177–215`, especially `:195–211`). Any thread that runs E5 callbacks must therefore carry the completion-turn flag, or a new switch appears.

### 1.5 How E5 completions travel

- **RouteMesh request replies.** The reply is drained by whichever thread runs `port->poll` for that ROUTER. On the mesh path that is only the node's receive thread: I0 `wait_for_activity` (`raw_mesh_node_owner.cpp:3478`) or `begin_receive_turn` (`:2679`). No other mesh caller of `observe_routes` / `try_receive` was found.
  - The binding drains completions inside `poller_t::wait` (`bindings/cpp/src/Runtime/Eventing/poller.cpp:396–404`).
  - That resumes `observe_request_completion` inline. Its scheduler is `{}` (`backend/raw_binding_adapter.hpp:30–36`, `:153–194`).
  - `source->complete` then runs the `observe_task_terminal` callback (`raw_mesh_node_owner.cpp:992–1055`), which calls `operations->complete` (`operation_registry.cpp:332–354`, `take` at `:314–325`).
  - That posts to the dispatcher. The `_worker` then runs `complete_operation` (`stateful/public_host_runtime.cpp:2865–2869`, `:7230–7290`), which completes the caller's task.
  - **Result: the receive thread does the terminal take; the switch to `_worker` is E5.**
- **Sends that came back `BACKPRESSURED`** use the same registry: registered at `raw_mesh_node_owner.cpp:1252–1274`, completed from the binding admission observer at `:1276–1290`. So they also cost one E5 switch each.
- **Timeouts** come from `expire_requests` inside `dispatch_ready` (`public_host_runtime.cpp:6136` → `operation_registry.cpp:437–455`), also on the receive thread.
- **Cancel, `fail_target` and shutdown** come from caller or teardown threads (`:356–364`, `:415–435`, `:469–492`).
- **ClientServer** (`backend/raw_dealer_port.cpp:112`, `channels/channel_host_service.cpp`) uses a different poller owner and its own handler executor. It is outside this design (see §4.3).

## 2. Designs

**Common part (used by A and B): the boundary decision and collecting E5 locally**

- **Role object.** Move the stack state from §1.1 into a heap `node_receive_role_t`: node, registration, `receive_permit`, `supply`, `pending_management`, a list of collected completions, and an atomic holder id. Its `rotate()` runs one loop iteration (`:2598–2798` unchanged) and returns at the boundary.
- **Ready owners come back to the holder.** At its own boundary, `end_application_receive_turn` returns the ready owners to the holder instead of calling `_application_ready`. Notifies from other threads (§1.2) keep calling `try_submit_internal` as today.
- **Collecting E5 locally.** `operation_completion_dispatcher_t::post` / `post_chain` check a thread_local pointer to the role's list. The holder sets that pointer only for the duration of `rotate()`. If it is set, the item is appended to the list (no lock; we are inside the binding drain stack). Otherwise it is posted as today. Collected items keep their drain slot.
- **One decision per boundary:**
  - Nothing collected and no ready owner: continue holding the role.
  - Otherwise, ask the placement owner to assign an idle resource.
    - **Success:** the old holder (no longer receiving) runs **one** unit as a new turn. That unit is the first collected completion, run with `completion_dispatcher_current = true` and the run-loop body's exception isolation and slot release (`:226–235`), or else the first ready owner's `drain_application_owner`. All remaining completions go to `post_chain` and all remaining owners to `try_submit_internal`, before the unit starts. The old holder then returns to idle.
    - **Failure:** `post_chain` all collected completions, submit all owners as today, keep the role.
- **What this preserves:**
  - Terminal take still happens where the completion arrives (spec §3 E5).
  - Registry FIFO within a rotation is kept. Spec S §11 does not require FIFO across operations (q1645r §4).
  - The nested-cancel test (`tests/Zlink.Framework.UnitTests/test_cpp_framework_operation_registry.cpp:303–320`) and the new-turn test (`:322–332`) are unaffected, because neither caller holds a role.
- **One new cost.** On the fallback path, completions are posted at the boundary instead of the instant they complete. That delay is at most one bounded rotation (≤64 records or 1 MiB, `dispatch_limits.hpp:10–11`).

### Design A — the host executor is the only placement owner; the role is a first-class pool item (recommended)

- **Who assigns, and how it is atomic.** `offload_executor_t` gains a role list next to `_queue`, plus `_role_active`.
  - `try_assign_role(role)` takes `_mutex`. It succeeds only if `!_stopping && _idle_workers > pending roles`. On success it pushes the role and calls `notify_one`.
  - `worker_loop` (`:221–253`) checks the role list **before** `_queue`. Its wait predicate, idle-retire test (`:235`) and stop-retire test (`:243`) all include "role list empty".
  - A role is popped by exactly one worker under `_mutex`. That worker is either parked in `_ready.wait` or between items. Either way it is running no application work, and it never waits behind `_queue`. This meets "exclusively assigned" and "does not wait for the application queue".
  - The role path never calls `start_worker_locked`, so no thread is created for a hand-off.
  - When the hand-off succeeds, the old holder moves from `_role_active` to ordinary `_active` under the same `_mutex`. Drain accounting then sees it as application work.
- **How the role moves.** The socket, poller, `_poller_mutex`, router (raw owner), permits (`receive_permit`, `supply`), management task and collected completions are all inside the role object. The handover point is the executor mutex: release by the old worker, acquire by the new one. The per-rotation budget is already returned at `:2757`, so no permit leaves its rotation. Permits already queued stay with their records and are released at handler entry, unchanged. Because the hand-off only happens at the boundary, the new holder starts the next I0/I2 on the same poller after the old wait/drain has returned.
- **Initial placement.** At start-up, the host pushes R role items (R = number of nodes), replacing `_threads`. The executor is built with min = R + 1 and max = R + max(2, hw) (change at `:683–687`). Peak thread count stays at R receive + max(2, hw) application + 1 dispatcher, same as today. The +1 in min means an idle target exists in steady state despite the 100 ms idle retirement.
- **If no idle resource exists,** the decision fails and today's path runs: owners go to `try_submit_internal`, completions go to the dispatcher, and the role stays. No timer, retry or extra queue.
- **Shutdown and close races:**
  - `stop()` currently drains the executor and then sets `_stop` (`:2985–2993`). With roles inside the executor, that drain would wait on the roles forever, and roles must stay alive during the drain so replies awaited by handlers can still arrive.
  - New sequence:
    1. `request_stop`.
    2. Drain application work only: `_queue.empty() && _active == _role_active`.
    3. Set `_stop` and `signal_dispatch_activity` for each node.
    4. Each role sees `_stop` at its boundary, calls `supply.close()`, `post_chain`s any remaining collected completions (never dropped; S §11), and returns.
    5. Wait for `_live_workers == 0` and join.
    6. `node->stop()` closes the port, as today.
  - `try_assign_role` fails while `_stopping`. `try_submit_internal`'s "still accepting" test (`:69`) must ignore `_role_active`, so the inline reject at `:2563` takes over after the application drain.
  - The swallowing at `:255–264` must not apply to roles. A rotation exception must be reported through `_runtime_failures` and must stop the host, or keep today's terminate behaviour (decision needed).
- **Expected switches per message** (role hand-offs counted separately):
  - Request-serial: server I4 = 0, client E5 = 0. There are 2 role hand-offs per round trip, each a condvar wake that runs alongside the handler or callback. Real cost needs measuring.
  - Send-saturation, server: an owner that is already draining is not marked ready again (`service_mailbox.cpp:66`, `:242–256`). A switch only happens on the extra owners of a rotation or on fallback: ≤ 1 per owner turn of up to 64 records, about 0.016.
  - Send-saturation, client: immediate admission gives 0. Sends that come back `BACKPRESSURED` give (k−1)/k when a rotation collects k of them (see §4.3).
  - Request-window: also (k−1)/k (see §4.3).
- **Files touched:**
  - `dispatch/offload_executor.{hpp,cpp}`
  - `mesh/mesh_node_host_service.{hpp,cpp}` (role object; remove `_threads`; stop order; constructor min/max)
  - `mesh/service_mailbox.{hpp,cpp}` (end-turn returns ready owners to the holder)
  - `foundation/operation_registry.{hpp,cpp}` (role collection hook; `run_one` / `publish` helpers)
  - Tests
- **Decision count:**
  - Thread owners on this path go from 3 (`_threads`, executor, dispatcher) to 2 (executor, dispatcher).
  - Placement decisions go from 2 (`:2558` submit, `:134–166` post) to 1 boundary decision, with the dispatcher kept as fallback.
  - Added: the role list, `_role_active`, one thread_local pointer. Removed: `_threads` and the per-node thread lambda.
- **Risk: medium-high.** The executor is shared code, used by `handlers/handler_registry.cpp:291`, `channels/channel_host_service.cpp:97,414` and others. The role list stays unused for them, but the `worker_loop` and `drain` changes affect them. Exception policy changes. The stop order changes.

### Design B — keep the per-node receive threads; pass the role between that thread and one pool worker

- **Who assigns.** Two placement owners: the host service owns `_threads[i]`, and the executor owns its workers. Each node has a role object plus a parked-standby slot.
  - If the role is on a pool worker, the target is the node's own `_threads[i]` when it is parked. The check is a CAS in the per-node state from `idle-parked` to `assigned`.
  - If the role is on `_threads[i]`, the target is an idle pool worker, using the A-style `try_assign_role` under the executor `_mutex`.
- **How the role moves:** as in A.
- **If no idle resource exists:** same fallback as A.
- **Shutdown:** `stop()` must recall a role currently on a pool worker before the executor drain, wake parked threads, and then join. That is two drains with a cross-owner handshake.
- **Switches:** same as A in steady state.
- **Resources:** there is no thread-count increase. But `_threads[i]` runs application turns outside the executor's max, so the application-concurrency peak rises by R. The spec checks total threads; concurrency limits would need re-checking.
- **Files:** as A, plus the per-node baton.
- **Risk: higher.** Two owners decide placement, which contradicts §4.4's "the existing placement owner" (singular) and the 3→1 direction in m1672c-plan ①. It adds a rule (the m1672c plan's "3→4" case). Shutdown is more complex. Kept for comparison only.

### Design C — read `_idle_workers`, then post a "continue receiving" item with `try_submit_internal` (rejected)

- The check and the post are not atomic. The posted item queues behind application items in the FIFO (`:72`, `:248`), and a worker coming back from busy can take an application item first.
- `try_submit_internal` may start a new worker (`:78–79`).
- This violates the spec text directly: "Executor에 게시했다는 사실이나 waiter가 있다는 사실만으로 성공으로 판정하지 않는다" (posting to an executor, or having a waiter, is not enough to count a hand-off as successful), and "새로 만들지 않는다" (do not create resources).

## 3. Recommendation, and the white-box checks from §4.4's last paragraph

**Recommend A.** It is the only option where one existing owner decides both "idle" and "exclusive" under one lock. It removes a thread owner instead of adding one, and it reaches 0 switches in request-serial and ≤ about 0.016 in server send-saturation.

**Order of work:** do it after ③②④⑤⑥ as the plan proposes. Start with the A executor change plus I4, then add the E5 collection, then measure.

**Checks** (trace build; record thread id per stage as in m1672c-plan §3):

1. **Progress while a handler or continuation is held.** Block every application worker, including the old holder, on a latch. Before releasing it, the new holder must still: answer a liveness probe (I3), complete an unrelated request (E5 to dispatcher), and run a management tick.
2. **An assigned role holder never runs application work.** A thread_local "holding a role" flag; assert in `drain_application_owner`, `dispatch_application` and registry callback entry that it is false.
3. **At most one receiver per socket.** CAS on the role's holder id at the start of `rotate()`; max concurrent rotations per node == 1. Assert no `_poller_mutex` is held at the boundary.
4. **Hand-off counters** (success and failure), recorded separately from switch counts.
5. **Thread peak.** `owned_thread_count()` ≤ R + max(2, hw); `start_worker_locked` calls made during `try_assign_role` == 0; dispatcher threads == 1.
6. **Switch counts.** I4 (claim → handler first instruction) and E5 (binding drain → continuation first instruction), per N = 10000: request-serial ≤ 0.1 each; send-saturation ≤ 0.1. When the old holder runs a completion, assert that `is_on_completion_dispatcher()` is true and that the continuation runs on the same thread.
7. **Fallback.** With every worker busy: hand-off fails, owners run from the executor queue, completions run on the dispatcher `_worker` thread id, and the role holder is unchanged.
8. **Shutdown races.** Stop during continuous hand-offs and in-flight requests: no hang; every collected completion is delivered (registry drain slots reach 0, shutdown terminals delivered); roles exit; all threads joined. Plus existing `test_cpp_framework_execution.cpp:9037,9072` (`live_worker_count() == 0` after drain).
9. **Existing tests stay green.** Registry `:303–332`; owner FIFO, gate and permit order (mesh vertical tests); permit still released at `release_for_handler_entry` (`:2549–2551`).

## 4. Open points

### 4.1 Cross-language
- From q1645r §2.2:
  - Java: the same hand-off is possible, but it needs a bounded worker structure first.
  - .NET: I4 is possible; E5 has a residual 1 because of the RCA pool notification.
  - Node: always keeps the current path.
- §4.4 already allows these differences, so it is not a gap in the spec.

### 4.2 Spec mapping
- Classification A: this applies the already-merged §4.4.

### 4.3 Decisions needed

1. **Completion independence with k > 1 collected completions.** Running the first and posting the rest satisfies "do not merge independent callbacks into one drain", but leaves (k−1)/k switches in request-window and for sends under HWM backpressure.
   - Running the collected list one after another on the old holder (A′) gives about 0, but makes callbacks depend on each other's return. The current single `_worker` already has that shape.
   - Which reading does the supervisor accept?
2. **Rotation exceptions.** Report and stop the host, or keep today's terminate?
3. **ClientServer is not covered.** `raw_dealer_port.cpp:112–129` and the `channel_host_service` threads need their own pass. q1645r notes the native continuation executor adds another switch there.

### Critical Files for Implementation
- `/root/worktree/zlink-1639-msg-cpp/framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp`
- `/root/worktree/zlink-1639-msg-cpp/framework/languages/cpp/framework/src/runtime/dispatch/offload_executor.cpp`
- `/root/worktree/zlink-1639-msg-cpp/framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp`
- `/root/worktree/zlink-1639-msg-cpp/framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp`
- `/root/worktree/zlink-1639-msg-cpp/framework/languages/cpp/framework/src/runtime/dispatch/coroutine_executor.cpp`
## 5. 감독자 결정 (2026-10-15)

- **설계 A 채택.** host의 `_application_dispatch`(offload_executor)를 유일한 배치 소유자로 하고 수신 역할을 그 worker가 `_queue`보다 먼저 가져가는 항목으로 만든다. `_threads` 제거. B는 배치 소유자가 둘이라(§4.4 "기존 실행 자원 배치의 소유자" 단수) 기각, C는 스펙 문장 위반으로 기각. 근거 코드 확인: `offload_executor.cpp:62-84`(FIFO 한 줄, 필요 시 worker 생성), `:221-266`(idle 계산·예외 삼킴), `mesh_node_host_service.cpp:683-687`.
- **4.3-1 (k>1 완료):** 엄격한 해석. 옛 holder는 **첫 번째 하나만** 새 turn으로 실행하고 나머지는 그 전에 `post_chain`으로 dispatcher에 넘긴다(A′ 기각 — 스펙 §4.4 표 "서로 무관한 완료 callback을 한 callback의 반환에 의존하는 drain으로 합치지 않는다"). window·HWM send의 (k−1)/k 잔여는 측정으로 기록하고 별도 판단.
- **4.3-2 (회전 예외):** 동작을 바꾸지 않는다. 지금 `std::thread` 수신 loop의 예외는 `std::terminate`이므로 역할 항목도 executor의 예외 삼킴(`:255-264`)을 적용받지 않고 같은 결과(terminate)를 유지한다. 바꾸려면 별도 이슈.
- **4.3-3 ClientServer:** 이번 범위 밖. RouteMesh 결과를 본 뒤 별도 단계.
- **순서:** (1) executor 역할 목록 + 수신 역할 객체 + I4 인계, 시험 3·5·7·8 (2) E5 수집 + 시험 1·2·6 (3) 횟수 계측(전환·인계 성공/실패·할당) — 시간 측정 없음.
- executor는 다른 사용처(handler_registry, channel_host_service)와 공유하므로, 역할 목록이 비어 있을 때 그 사용처의 동작(대기 조건·retire·drain)이 바뀌지 않음을 시험으로 보인다.
