# Bucket B C++ m6b late-section investigation summary

## Result

The two late failures have different causes. In the initial five full-binary
runs, the raw bound-session completion failure reproduced 4/5 times and the
route-cache owner-admission failure reproduced 1/5 times. This job therefore
fixed the raw routing case and retained the route-cache timing failure as a
separate candidate.

The raw routing regression now uses a real `request_to_actor` transport request
with the bound-session tail. Its focused test passed 30/30. On final source,
the full m6b binary passed the repaired section in all four runs that reached
it; four runs completed and one stopped earlier at the independent route-cache
assertion.

## Per-run stops

Initial full m6b runs used both existing traces and a 120-second timeout.

| Run | Exit | Elapsed | Stop |
|---:|---:|---:|---|
| 1 | 134 | 6 s | `verify_raw_spot_and_actor_routing`, old line 4524, bound delivery `complete_async` |
| 2 | 134 | 6 s | `verify_raw_spot_and_actor_routing`, old line 4524, bound delivery `complete_async` |
| 3 | 134 | 7 s | `verify_raw_spot_and_actor_routing`, old line 4524, bound delivery `complete_async` |
| 4 | 134 | 6 s | `verify_raw_spot_and_actor_routing`, old line 4524, bound delivery `complete_async` |
| 5 | 134 | 3 s | `verify_public_host_route_cache_stops_at_owner_admission_deadline`, line 1909 |

Logs: `/tmp/zlink-cpp-m6b-initial.u0qdmW/run-{1..5}.log`.

Final-source full m6b runs:

| Run | Exit | Elapsed | Stop |
|---:|---:|---:|---|
| 1 | 0 | 6 s | completed |
| 2 | 0 | 6 s | completed |
| 3 | 0 | 6 s | completed |
| 4 | 0 | 6 s | completed |
| 5 | 134 | 3 s | route-cache owner-admission assertion, line 1909 |

Logs: `/tmp/zlink-cpp-m6b-final-source.ikcCM8/run-{1..5}.log`.

## Root cause: raw bound-session completion

The failing setup was in
`framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp`
at the old lines 4463-4476. It directly inserted an `actorRequest` mailbox
record and set its opaque `reply_token` to `std::nullopt`.

This became invalid when commit `b32d4cae64` migrated C++ to the 0.16.0
pull-completion model. That migration replaced the test's numeric fake reply
token with `std::nullopt` but left the test expecting a successful reply.
The normal wire receive path never creates such a record: an actor request
must arrive with a Core-issued opaque reply token.

Evidence from the existing mesh trace and the implementation:

- The preceding normal actor request reached `receive kind=25 ...
  replyToken=present`, application enqueue, and `request-completion ...
  result=0`.
- The synthetic bound-session request produced no new request receive
  transition because it bypassed the transport.
- `raw_stateful_dispatch_t::complete_async` at
  `framework/languages/cpp/framework/src/runtime/stateful/raw_stateful_dispatch.cpp:679-718`
  passed the synthetic mailbox record to the transport reply path.
- `raw_mesh_node_owner_t::reply` at
  `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1597-1619`
  returned `false` at line 1601 because the reply token was absent.
  `complete_async` consequently returned `stateful_error_t::conflict` at
  lines 709-714. This is the exact transition that stopped.

The production guard is correct and was not weakened. A reply cannot be
submitted without the opaque token issued for the received request.

## Spec clause matched

`framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:31-49`
requires request replies to use the preserved reply route and correlation.
`framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:289-293`
requires bound-session command 24 to retain the existing request correlation
and deadline contract, and lines 722-731 require the Actor reply to complete
the original request exactly once.

The repaired test now exercises that specified route instead of constructing
a tokenless state that cannot come from the normal receiver.

## Minimal fix and regression

Only
`framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6b_runtime.cpp:4463-4555`
was changed for this cause.

- The test submits the bound-session actor request through the existing
  `raw_mesh_node_owner_t::request_to_actor` API, including the exact operation,
  bound-session tail, and correlation.
- It pumps the two already-connected raw mesh owners until the target receives
  the application record.
- Existing frozen identity and canonical-journal assertions remain unchanged.
- After `complete_async`, it verifies the source callback completed and decoded
  the exact `bound-reply` payload.

No assertion or sleep was relaxed. No production API, Core, binding, or
protected spec/site file was changed. Temporary test selection and timing
traces were removed.

Focused regression command:

```bash
cmake --build framework/languages/cpp/build/linux-ninja-debug -j16 \
  --target test_cpp_framework_m6b_runtime
cd framework/languages/cpp/build/linux-ninja-debug
for run in $(seq 1 30); do
  ZLINK_CPP_MESH_TRACE=1 \
  ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1 \
  timeout 10s ./test_cpp_framework_m6b_runtime
done
```

The main function was temporarily narrowed to
`verify_raw_spot_and_actor_routing` for this command and restored afterward.
Result on the final regression source: 30/30 pass. Logs:
`/tmp/zlink-cpp-raw-routing-final.DeatDY/run-{1..30}.log`.

## Route-cache candidate at line 1909

This is category **(b), a clock/timing flake**, not category (a), an ENOENT
retry extending beyond the three-second window.

Focused evidence:

- With only the route-cache test selected, 2/15 runs failed (runs 2 and 12).
- In a further 20 runs, failures were runs 7 and 16. Temporary result capture
  showed the second `send()` returned `submit_result_t::ok` (`0`), not a
  retry/deadline result.
- Timing capture on a failing run showed the initial cache lifetime was
  1,973 ms, honoring the five-second owner lease minus the configured
  three-second fencing margin. Three seconds later the steady-clock cache
  probe was 1,027 ms past expiry, so the cache correctly evicted the entry.
  The subsequent Store read nevertheless reported another 1,702 ms of
  admission lifetime for the same unrenewed lease, and the live raw target
  accepted the send.

The path is
`public_host_runtime_t::resolve_spot_route_fence` at
`framework/languages/cpp/framework/src/runtime/stateful/public_host_runtime.cpp:2471-2530`.
It stores the first admission lifetime against `steady_clock`, as required by
the spec, and evicts it at lines 2497-2503. The in-memory repository creates
and rereads the lease using `system_clock` at
`framework/languages/cpp/framework/src/runtime/locations/in_memory_location_store.hpp:429-489`;
`live_location_reader_t` converts each fresh system-clock remainder to a
steady duration at
`framework/languages/cpp/framework/src/runtime/locations/live_location_reader.hpp:53-64`.
An intermittent backward wall-clock adjustment between those reads therefore
makes the same unrenewed lease appear admissible again.

This evidence rules out ENOENT: the second call acquired a fresh fence and
completed its raw send with `ok`, so the dead-target bounded retry path was
never entered. No sleep was added or extended. The normative cache rule at
`framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md:111-128`
says a cached Ready route is usable only until the earlier of local owner
admission deadline and `RouteCacheMaxAge`; the observed first cache entry did
exactly that. The cross-clock test/store behavior remains a separate candidate.

Focused logs:

- `/tmp/zlink-cpp-route-cache-focused.dXcQov/run-{1..15}.log`
- `/tmp/zlink-cpp-route-cache-result.E1850V/run-{1..20}.log`
- `/tmp/zlink-cpp-route-cache-timing.WXutiy/run-10.log`

## Verification commands

All binary runs were from the designated build directory only.

```bash
cmake --build framework/languages/cpp/build/linux-ninja-debug -j16 \
  --target test_cpp_framework_m6b_runtime
cd framework/languages/cpp/build/linux-ninja-debug
for run in 1 2 3 4 5; do
  ZLINK_CPP_MESH_TRACE=1 \
  ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1 \
  timeout 120s ./test_cpp_framework_m6b_runtime
done
```

Final m6b result: 4/5 complete; run 5 stopped at the independent line-1909
candidate before reaching raw routing. The repaired raw routing section passed
4/4 full runs that reached it and 30/30 focused runs.

```bash
cmake --build framework/languages/cpp/build/linux-ninja-debug -j16 \
  --target test_cpp_framework_m6a_runtime
cd framework/languages/cpp/build/linux-ninja-debug
ZLINK_CPP_MESH_TRACE=1 \
ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1 \
timeout 120s ./test_cpp_framework_m6a_runtime
```

Final m6a result: exit 0 in 2 seconds. A separate plain-hello job changed that
test while this job was running, so the previously known
`verify_client_server_plain_hello_is_rejected` stop did not reproduce. Log:
`/tmp/zlink-cpp-m6a-final.64Scxn.log`.

## BLOCKERS

- The line-1909 route-cache test remains an intermittent cross-clock candidate.
  It stopped 1/5 final full m6b runs. Fixing it would be a second cause and is
  outside this one-cause job; do not revert the ENOENT classification or add a
  longer sleep.
- There is no Core or binding blocker for the repaired raw bound-session case.
- Existing unrelated working-tree changes, including the two established C++
  fixes, were preserved. No commit was created.
