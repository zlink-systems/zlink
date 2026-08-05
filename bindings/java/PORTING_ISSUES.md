# Java Porting Issue Report

## Scope
This report covers issues found while porting core tests to `bindings/java` without skip/disable workarounds.

## Core Test Porting Coverage (default `core/tests/CMakeLists.txt` set)
- Core default test tokens: `55`
- Ported in Java: `52`
- Remaining not ported: `3`

## Remaining not ported (and why)
### 1) `test_ancillaries`
- Reason: C-level ancillary/FD control path is not exposed through current Java binding API surface.

### 2) `test_msg_ffn`
- Reason: `zlink_msg_init_data` free-function callback ownership path is not exposed in Java `Message` API.

### 3) `test_zmp_metadata`
- Reason: C API metadata accessor path (`zlink_msg_gets`) is not exposed in Java `Message`.

## Resolved during this pass
### Newly ported from `core/tests`
- `test_connect_resolve` -> `TestConnectResolvePortedTest`
- `routing-id/test_connect_rid_string_alias` -> `TestConnectRidStringAliasPortedTest`
- `test_socket_null` -> `CoreSocketNullPortedTest`
- `test_pair_send_blocking_wakeup` -> `TestPairSendBlockingWakeupPortedTest`
- `test_bind_src_address` -> `TestBindSrcAddressPortedTest`
- `test_issue_566` -> `TestIssue566PortedTest`
- `test_shutdown_stress` -> `TestShutdownStressPortedTest`
- `test_spec_router` -> `TestSpecRouterPortedTest`
- `test_stream_fastpath` -> `TestStreamFastpathPortedTest`
- `test_stream_send_blocking_wakeup` -> `TestStreamSendBlockingWakeupPortedTest`
- `test_zmp_ws_wss` -> `TestZmpWsWssPortedTest`

## Validation status
- `./gradlew test --no-daemon`: pass
- Targeted new-port tests: pass (class-level runs)
- `./gradlew integrationTest --no-daemon`: process-abort observed in full-suite run:
  - Assertion: `!has_out_pipe (routing_id)` at `core/src/sockets/router.cpp:426`
  - This abort is reproducible in full-suite execution.
