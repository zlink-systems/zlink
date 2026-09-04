# Bucket B C++ m6a plain-hello timeout summary

## Result

`verify_client_server_plain_hello_is_rejected()` no longer hangs. The failure
was in the test harness, not in the ClientServer server, C++ binding, or Core.
The test blocked on a binding DONTWAIT send while the poller that owned that
socket's completion lane was never waited. The test now drives the binding's
public completion poller until the send task settles, then retains the original
assertions that the hello was sent and that the server returns
`client_server_pump_result_t::protocol_error`.

The focused regression passed 20/20, the final-source full m6a binary passed
3/3, and the final-source full m6b binary passed 1/1.

## Hang location and backtrace

The function was temporarily isolated at the start of `main()` and the
isolation was removed before final verification. An isolated run produced no
trace output and timed out after 20 seconds:

```text
timeout 20s env ZLINK_CPP_MESH_TRACE=1 \
  ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1 \
  ./test_cpp_framework_m6a_runtime
exit 124
```

Interactive gdb was interrupted after the hang and `thread apply all bt 30`
showed the main thread waiting for the send task itself. It had not reached the
server admission/pump loop:

```text
#5  std::condition_variable::wait<...>()
#6  zlink::framework::detail::task_shared_state_t<bool>::result()
    at framework/include/zlink/framework/contracts/dispatch/task.hpp:309
#7  zlink::framework::task_t<bool>::result()
    at framework/include/zlink/framework/contracts/dispatch/task.hpp:475
#8  (anonymous namespace)::await_task<bool>()
    at test_cpp_framework_m6a_runtime.cpp:47
#9  verify_client_server_plain_hello_is_rejected()
    at test_cpp_framework_m6a_runtime.cpp:1516
#10 main() at test_cpp_framework_m6a_runtime.cpp:2937
```

The pre-fix line 1516 was
`await_task(port.send(hello_message))`. Therefore the hang was the client's
`raw_dealer_port_t::send` await, not the server admission loop or the test's
post-send wait loop.

## Root cause and ownership

Classification: **test harness**.

- `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp:1505-1516`
  constructed `raw_dealer_port_t` with its private poller and immediately made
  a blocking `task_t::result()` wait on `port.send()`.
- `framework/languages/cpp/framework/src/runtime/backend/raw_dealer_port.cpp:22-41`
  registers the DEALER for `POLLOUT | POLLCOMPLETION`. On the first send, the
  physical target can still be unready, so Core returns BACKPRESSURED plus a
  nonzero wait token and the binding awaitable remains pending.
- `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:369-387` explicitly
  assigns SEND retry progress to the application's public poller and disables
  the private REQUEST fallback owner while a send retry exists.
- `bindings/cpp/src/Runtime/Eventing/poller.cpp:520-547` drains the C++ binding
  completion owner from `poller_t::wait()`, which captures WRITABLE, resubmits,
  and settles the awaitable before returning the completion event.

The old test never called that poller's `wait()`. Its outer five-second send
deadline could not help because execution was blocked inside the first
`await_task()` and never returned to check the deadline.

This is not a binding defect: `bindings/doc/spec/cpp/README.ko.md:790-797`
requires a wait loop when a public poller owns completion-backed progress. It
is not a framework server defect either: after delivery,
`framework/languages/cpp/framework/src/runtime/client_server/raw_client_server_owner.cpp:577-579`
already classifies this otherwise invalid ClientServer service-wire command as
`protocol_error`.

Two alternatives were rejected:

- Making `raw_dealer_port_t::send()` start a hidden completion thread would
  introduce a second progress owner and conflict with the binding's one-public-
  poller ownership model.
- Changing to the binding's blocking `submit()` would avoid the C++ framework's
  actual DONTWAIT path and stop this test from exercising the same transport
  adapter used by the runtime.

## Specification match

Service wire spec 51 section 4, ClientServer direction
(`framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md:346-349`)
requires:

- the client to originate `hello` as a Core request;
- the server to return `admit` or `reject` only on that request's reply leg;
- RouteMesh records reused on a ClientServer connection, or the reverse, to
  end as protocol error.

The assertion remains unchanged. The plain send is delivered, and the existing
server branch returns protocol error rather than admitting it.

## Fix

Only the test harness changed:

- `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_m6a_runtime.cpp:1505-1527`
  now supplies a shared `zlink::poller_t` to `raw_dealer_port_t`, starts one
  send task, and waits that poller in 10 ms bounded turns until the task settles
  or the existing five-second deadline expires.
- The existing `assert(sent)` and final `assert(result == protocol_error)` were
  not lowered or changed.
- No production framework, binding, Core, protected spec, or site file was
  modified for this cause. No temporary trace or `main()` isolation remains.

## Verification

Commands were run from the repository root unless a build-directory `cd` is
shown.

1. Build and initial isolated reproduction:

   ```bash
   cmake --build framework/languages/cpp/build/linux-ninja-m6a -j16 \
     --target test_cpp_framework_m6a_runtime
   cd framework/languages/cpp/build/linux-ninja-m6a
   timeout 20s env ZLINK_CPP_MESH_TRACE=1 \
     ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1 \
     ./test_cpp_framework_m6a_runtime
   gdb -q --args ./test_cpp_framework_m6a_runtime
   # run; Ctrl-C; thread apply all bt 30
   ```

   Result before the fix: timeout exit 124; gdb stopped at the send task wait
   shown above.

2. Focused regression with temporary `main()` isolation, subsequently removed:

   ```bash
   for run in $(seq 1 20); do
     timeout 10s ./test_cpp_framework_m6a_runtime || exit $?
   done
   ```

   Result: 20/20 pass, including the unchanged server protocol-error
   assertion.

3. Final-source full m6a:

   ```bash
   cmake --build framework/languages/cpp/build/linux-ninja-m6a -j16 \
     --target test_cpp_framework_m6a_runtime
   cd framework/languages/cpp/build/linux-ninja-m6a
   timeout 180s ./test_cpp_framework_m6a_runtime
   timeout 180s ./test_cpp_framework_m6a_runtime
   timeout 180s ./test_cpp_framework_m6a_runtime
   ```

   Result: 3/3 pass, exit 0.

4. Final-source full m6b:

   ```bash
   cmake --build framework/languages/cpp/build/linux-ninja-m6a -j16 \
     --target test_cpp_framework_m6b_runtime
   cd framework/languages/cpp/build/linux-ninja-m6a
   timeout 180s ./test_cpp_framework_m6b_runtime
   ```

   Result: 1/1 pass, exit 0.

## BLOCKERS

None for this job.

