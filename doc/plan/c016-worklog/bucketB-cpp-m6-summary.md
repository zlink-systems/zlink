# Bucket B C++ m6a/m6b investigation summary

## Result

The ENOENT route-absence retry is working. The requested
`verify_bound_session_bind_retries_until_route_is_admitted` scenario admitted
both peers, resent command 38, and completed; an isolated run passed 30/30.
The 75 ms retry timer does not monopolize `pump_one` or the ROUTER mutex:
`infrastructure_request_retry_state_t::schedule_retry()` arms an asynchronous
timer, and `raw_route_port_t::request()` holds `_socket_mutex` only while it
creates the native request.

The reproducible m6a abort after that scenario was a separate same-endpoint
replacement defect. GDB identified the caller of `admit_pair()` line 155 as
`verify_stale_rid_disconnect_preserves_same_endpoint_replacement()` line 746
(line 742 before the regression assertion was added), not the delayed bind
scenario.

## Root cause and evidence

The owning symbol was
`framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:787`,
`raw_mesh_node_owner_t::connect_peer(endpoint, expected_descriptor)`.

Before the fix, it assigned a new `connect_routing_id` and called `connect()`
on an endpoint for which the ROUTER still had the old reconnect intent. Core's
connect-routing-id is captured when a new connect intent is created; calling
`connect()` again for the already configured endpoint did not retarget that
intent. The existing mesh trace showed:

- `connect ... expected=replacement-new`
- the following `connection_ready` still reported routing
  `replacement-old`
- no `replacement-new` hello/admit transition occurred before the 2 s
  admission deadline

After retiring and recreating the endpoint intent, the same trace reported
`connection_ready ... routing=replacement-new`, followed by local admission
and bilateral-ready.

Two alternatives were rejected:

- Changing the pump cadence or increasing the test deadline would only hide
  the wrong RID; the trace showed that the pump was progressing.
- Reinterpreting an old-RID monitor event as the expected new RID would invent
  peer identity in the framework and weaken admission fencing.

## Fix

- `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:797-828`
  detects an existing expectation for a different RID at the same endpoint.
  If that endpoint has an outbound connect intent, it disconnects the stale
  intent before setting the replacement `connect_routing_id` and reconnecting.
  It then removes stale same-endpoint expectations and records the new one.
  This is minimal because it changes only the C++ RouteMesh connection owner
  and leaves Core/bindings untouched.
- `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp:735-738`
  strengthens the existing replacement regression with the essential
  precondition that the old RID has actually left topology before the same
  endpoint is retargeted. The complete replacement test passed 20/20 in
  isolation.
- The pre-existing ENOENT regression remains at
  `test_cpp_framework_m6a_runtime.cpp:2938-2946`, and ENOENT remains transient
  in `raw_binding_adapter.hpp`.
- The stale EHOSTUNREACH wording was corrected to the observed ENOENT (2) in
  `test_cpp_framework_m6b_runtime.cpp:1305-1324` and
  `raw_route_port.cpp:159-176`. No assertion was changed.

All temporary test-selection returns and stderr stage traces were removed.

## Verification

Commands were run from the repository root unless noted otherwise.

1. Initial focused reproduction:

   ```bash
   cmake --build framework/languages/cpp/build/linux-ninja-debug -j16 \
     --target test_cpp_framework_m6a_runtime
   cd framework/languages/cpp/build/linux-ninja-debug
   ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1 ./test_cpp_framework_m6a_runtime
   ZLINK_CPP_MESH_TRACE=1 ./test_cpp_framework_m6a_runtime
   gdb -q -batch -ex run -ex 'bt 20' --args ./test_cpp_framework_m6a_runtime
   ```

   Result: spot-discovery had no output for this control path. Mesh tracing
   showed the delayed bind succeeding and the replacement connection keeping
   the old RID. GDB resolved the line-155 helper assertion to the replacement
   test's call site.

2. Delayed-bind scenario isolated temporarily, then all temporary edits
   removed:

   ```bash
   for run in $(seq 1 30); do ./test_cpp_framework_m6a_runtime; done
   ```

   Result: 30/30 pass.

3. Same-endpoint replacement regression isolated temporarily, then all
   temporary edits removed:

   ```bash
   for run in $(seq 1 20); do timeout 10s ./test_cpp_framework_m6a_runtime; done
   ```

   Result: 20/20 pass.

4. m6b retry classification isolated temporarily, then all temporary edits
   removed:

   ```bash
   cmake --build framework/languages/cpp/build/linux-ninja-debug -j16 \
     --target test_cpp_framework_m6b_runtime
   for run in 1 2 3; do timeout 30s ./test_cpp_framework_m6b_runtime; done
   ```

   Result: 3/3 pass, including
   `transport_failure.error_kind() == deadline_exceeded` at line 1343.

5. Required adjacent target:

   ```bash
   cd framework/languages/cpp/build/linux-ninja-debug
   cmake --build . -j16 --target test_cpp_framework_channel_messaging
   timeout 180s ./test_cpp_framework_channel_messaging
   ```

   Result: pass (exit 0).

6. Final-source full binaries:

   ```bash
   cmake --build framework/languages/cpp/build/linux-ninja-debug -j16 \
     --target test_cpp_framework_m6a_runtime test_cpp_framework_m6b_runtime
   cd framework/languages/cpp/build/linux-ninja-debug
   timeout 60s ./test_cpp_framework_m6a_runtime
   timeout 60s ./test_cpp_framework_m6b_runtime
   ```

   Result: blocked by the unrelated failures below. Consequently the requested
   full-binary 3x green result was not attainable in this one-cause job.

## BLOCKERS

- Full m6a passes the delayed-bind and same-endpoint replacement sections but
  does not finish. Temporary stage markers narrowed the stop to
  `verify_client_server_plain_hello_is_rejected()`; that function also timed
  out at 20 s when run alone. The markers and isolation return were removed.
  This is independent of the infrastructure retry and requires a separate
  client/server raw-handshake job.
- Full m6b passes `verify_remote_bound_session_bind_classifies_retryable_outcomes`
  but later remains unstable. One run aborted at
  `verify_raw_spot_and_actor_routing()` line 4524; the final-source run aborted
  at `verify_public_host_route_cache_stops_at_owner_admission_deadline()` line
  1909 (`send() == zlink::submit_result_t::not_found`). These later failures
  were not modified or expectation-lowered.
- No spec change is proposed. No file under the protected spec/site trees was
  touched.

