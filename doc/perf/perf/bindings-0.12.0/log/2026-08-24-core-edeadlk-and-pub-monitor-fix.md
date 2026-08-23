# Core EDEADLK enforcement 및 PUB monitor full-mask close fix

작업 범위: `core/`만. 최종 빌드·검증은 `core/build-sendfix`에서 수행했다.
`core/build`는 변경하지 않았고 commit도 만들지 않았다.

## 1. EDEADLK enforcement

### Root cause

- 기존 `socket_send_complete_dispatch_scope_t`는
  `core/src/runtime/sockets/common/socket_dispatch_bridge.cpp:9-13,50-80`의
  thread-local scope로 이미 send-complete callback을 표시하고 있었다. 하지만
  `core/src/runtime/sockets/common/socket_send_complete.cpp`의 기존
  `:582-588` 직접 검사만 이 TLS를 읽었다.
- 따라서 동기 send/publish/request 진입은
  `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp:43-69`의
  closing-state admission만 통과했고, 특히 `:55-61`의 uncontended CAS 경로는
  callback 재진입을 검사하지 않았다.
- reply callback은
  `core/src/api/socket/request_completion_queue_internal.cpp:18-31`의
  request-owner TLS만 사용했으므로 send-complete dispatch TLS 안에 있지 않았다.
  다른 socket으로의 재진입도 막히지 않았다.

계약에 맞게 의미는 socket 단위가 아니라 **global TLS**로 유지했다. 어느 socket의
Core completion callback 안에서든 어느 socket으로 submit해도 `EDEADLK`가 되어야
한다.

### Fix

- `socket_lifecycle_runtime.cpp:43-71`의 두 public admission 경로
  (`enter_public_api`, `enter_public_api_and_lock_sync`) 앞에 global TLS guard를
  넣었다. 동기 sync fast path도 같은 guard를 거친다.
- `socket_send_complete.cpp:555-561`에서 `zlink_send_async`도 공통 admission을
  send-async 인자 검증보다 먼저 수행하게 했다. 따라서 callback 대상이 다른
  socket이고 그 socket에 completion handler가 없어도 먼저 `EDEADLK`가 반환된다.
- `request_completion_queue_internal.cpp:21-32,107-116`의 실제 reply handler
  호출을 기존 request-owner scope와 `socket_send_complete_dispatch_scope_t`로
  함께 감쌌다. scope의 이전 TLS를 복원하므로 nested dispatch도 보존된다.
- `socket_runtime.hpp:613-618`에 send-complete와 reply callback이 공유하는
  global TLS 의미를 기록했다. 기존 `completion_drain_scope_t`는 callback
  invocation 바깥의 owner 관리 역할을 계속 맡고, callback 자체만 공통 dispatch
  scope에 들어간다.

### Tests

- `test_send_async_multipart.cpp:418-480`:
  send-complete callback에서 같은/다른 socket의 sync send, 같은/다른 socket의
  `zlink_send_async`, publish, request를 호출한다. 모두
  `ZLINK_SUBMIT_THREAD_VIOLATION`과 `errno=EDEADLK`를 확인한다. 원래 async
  operation의 callback은 `ZLINK_SEND_ADMITTED`, non-zero op id로 완료되어
  callback completion 경로가 살아 있음을 함께 확인한다.
- `test_zmp_request_reply.cpp:750-800`:
  reply callback에서 같은/다른 DEALER의 sync request와 `zlink_send_async`를
  호출하고 네 경우 모두 `ZLINK_SUBMIT_THREAD_VIOLATION`/`EDEADLK`를 확인한다.
- 기존 multipart positive cases를 포함한 `test_send_async_multipart`는 5/5
  통과했고, 새 reply callback case도 1/1 통과했다.

## 2. PUB monitor full-mask close SIGSEGV

### Reproducer and root cause

먼저 `core/tests/integration/test_pub_monitor_close_full_mask.c`를 최소 C
reproducer로 만들었다. handler를 설치하지 않고 PUB socket에
`options.events = 0x7FFFFu`로 monitor를 연 뒤 PUB만 닫고 reaper가 동작할 시간을
준다. 수정 전 pre-built `core/build/lib`에 read-only로 링크해 실행하면
`opened`, `closing`, `closed` 뒤 exit 139(SIGSEGV)였다. `NULL` options 경로는
문제를 재현하지 않았다.

정확한 UAF 경로는 다음과 같다.

1. 수정 전 `core/src/runtime/sockets/common/socket_base.cpp:326-335`에서
   socket의 `_mailbox`를 먼저 삭제했다.
2. 이어서 `stop_monitor()`가
   `core/src/runtime/sockets/common/socket_base_monitor.cpp:747-793`의
   `detach_monitor_socket()`를 통해 monitor의 async-command ownership을
   해제했다.
3. ownership 해제 경로는 `stop_async_mailbox_processing()`으로 들어가
   `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:263-272`에서
   이미 해제된 `_mailbox`에 `signal()`을 호출했다.

즉 full event mask가 teardown 중 monitor async hand-off를 활성화하면서 reaper
thread가 socket의 `_mailbox`를 use-after-free한 것이며, monitor event record나
pipe 자체가 dangling object인 것은 아니었다.

### Fix

`socket_base.cpp:326-337`에서 기존 monitor runtime lock을 유지한 채
`stop_monitor()`를 먼저 호출하고 `_mailbox`를 마지막에 삭제하도록 순서를
바꿨다. monitor async ownership hand-off와 mailbox signal이 끝날 때까지
mailbox lifetime이 보장된다. C regression test는 `core/tests/CMakeLists.txt`
의 `test_pub_monitor_close_full_mask` target/test로 등록했다.

## 검증 결과

### Targeted

```text
cmake --build core/build-sendfix --target \
  test_send_async_multipart test_zmp_request_reply \
  test_pub_monitor_close_full_mask -j2
PASS

ctest --test-dir core/build-sendfix \
  -R '^(test_send_async_multipart|test_pub_monitor_close_full_mask)$' \
  --output-on-failure
100% tests passed, 0 tests failed out of 2

env LD_LIBRARY_PATH=core/build-sendfix/lib \
  ZLINK_TEST_CASE=test_reply_callback_rejects_sync_and_async_submit_on_all_sockets \
  core/build-sendfix/bin/test_zmp_request_reply
1 Tests 0 Failures 0 Ignored — PASS
```

최종 C reproducer를 `core/build-sendfix` library로 단독 실행해 exit 0을
확인했고, CTest 결과도 2.01초에 통과했다. 수정 전 read-only baseline
reproducer는 exit 139였다.

### Full CTest comparison

```text
ctest --test-dir core/build-sendfix -j8 --output-on-failure
39 passed / 54 failed / 93 total (42% passed)
```

새 테스트 `test_send_async_multipart`(#40),
`test_pub_monitor_close_full_mask`(#50)는 모두 통과했다. 실패한 54개는
다음의 기존 환경·작업 트리 조건으로 분류됐다.

- sandbox가 TCP/IPC/AF_INET bind를 `errno=1 (Operation not permitted)`로
  거부해 발생한 transport 및 그에 따른 cleanup/wait timeout;
- 기존 `unittest_poller`의 `Expected 0 Was 1` 환경성 실패;
- 기존 `test_thread_safe_contract_policy`가 요구하는
  `core/doc/internals/threading-model.en.md` 미존재;
- 기존 request/reply suite의 generic DEALER receive payload-queue expectation
  failure;
- 위 실패에 종속된 request/monitor teardown timeout.

따라서 이번 변경에 해당하는 새 테스트 실패는 없었다. 사용자가 보호한
`core/build`는 pristine full baseline 비교를 위해 건드리지 않았고, 그 대신
수정 전 PUB reproducer만 기존 library에 read-only 링크해 SIGSEGV를 확인했다.

### Grep / hygiene

```text
rg -n "send_ready" core/src core/include core/tests
(no output)

git diff --check
(no output)
```

## 변경 파일

- `core/src/api/socket/request_completion_queue_internal.cpp`
- `core/src/runtime/sockets/common/socket_base.cpp`
- `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp`
- `core/src/runtime/sockets/common/socket_runtime.hpp`
- `core/src/runtime/sockets/common/socket_send_complete.cpp`
- `core/tests/CMakeLists.txt`
- `core/tests/integration/test_send_async_multipart.cpp`
- `core/tests/integration/test_pub_monitor_close_full_mask.c`
- `core/tests/integration/test_zmp_request_reply.cpp`
- `doc/perf/perf/bindings-0.12.0/log/2026-08-24-core-edeadlk-and-pub-monitor-fix.md`
