# Core `zlink_send_async` routed multipart 수정 기록

## 범위와 빌드 격리

- 대상: `core/`의 `ROUTER`·`DEALER` `zlink_send_async` multipart admission
- `core/build`는 사용하지 않고 별도 디렉터리만 사용했다.
- 빌드: `cmake -B core/build-sendfix -S core -DCMAKE_BUILD_TYPE=Release`
- commit과 push는 하지 않았다.

## 재현

작업 시작 시 작은 C 재현 프로그램을 `/tmp/zlink_sendfix_repro.c`에 만들고
`core/build-sendfix/lib/libzlink`에 링크했다.

수정 전 결과:

- `ROUTER` exact target, 2-part `zlink_send_async`:
  `router_send_path.cpp:215`의 `zlink_assert (!_more_out)`로 프로세스가
  abort했다(exit 134).
- `DEALER`, `target == NULL`, 2-part record: completion이 terminal route
  failure로 끝났고, `send_async` 호출 자체는 ownership을 넘긴 뒤 성공했지만
  record가 전달되지 않았다.
- 같은 `DEALER` 상태에서 1-part record는 admission과 전달이 성공했다.

수정 후 같은 재현을 다시 실행한 결과:

```text
ROUTER 2-part: completion result=0 errno=0, send_async result=0
DEALER target=NULL 2-part: completion result=0 errno=0, send_async result=0
DEALER target=NULL 1-part: completion result=0 errno=0, send_async result=0
```

## 원인

### D1: ROUTER multipart가 assertion으로 종료됨

`core/src/runtime/sockets/common/socket_send_complete.cpp:261-280`의 기존
admission loop는 routed record의 모든 part에 `send_direct_with_retry(&rid, ...)`를
호출했다. 따라서 2번째 part도 `router_t::xsend_routed`에 들어갔다.

첫 part가 `SNDMORE`이면
`core/src/runtime/sockets/router/router_send_path.cpp:215-224`에서
`_more_out`과 `_current_out`이 이미 multipart 진행 상태가 된다. 이 상태에서
다시 message-start API인 `xsend_routed`를 호출하면
`zlink_assert (!_more_out)`가 public async input에 의해 발생했다.

### D2: DEALER multipart가 1-part와 다르게 route failure가 됨

DEALER의 routed entry는
`core/src/runtime/sockets/dealer/dealer.cpp:170-173`에서 선택된 pipe에
`lb_t::sendpipe_to`를 호출한다. 첫 multipart part가 성공하면
`core/src/runtime/sockets/internal/lb.cpp:310-311`에서 `_more`와
`_weighted_multipart_pipe`가 설정된다. admission loop가 다음 part에도
`sendpipe_to`를 다시 호출하면서 `lb.cpp:272-274`의 `_more` 검사에 걸려
`EFSM`이 된다. 이 오류가 async completion의 route failure 결과로 관찰되어
`target == NULL` DEALER의 multipart만 NOT_FOUND/EHOSTUNREACH처럼 보였다.

`target == NULL`의 DEALER target 자체는 기존 계약대로 submit 시점에
`socket_send_complete.cpp:599-629`에서 한 번 선택된다. 이 수정은 연결이 없는
경우의 기존 `NOT_CONNECTED` 정책을 바꾸지 않고, 이미 선택된 target을
multipart의 첫 part에서만 사용한다.

## 수정

`core/src/runtime/sockets/common/socket_send_complete.cpp:240-280`에서 다음처럼
변경했다.

1. record 전체는 기존처럼 하나의 `socket_public_send_scope_t` 안에서 처리한다.
2. routed record의 첫 part만 exact RID/pair/generation으로
   `xsend_routed`에 보낸다.
3. 후속 part는 target 인자를 `NULL`로 하여 socket의 일반 `xsend` 경로에
   보낸다. ROUTER의 `_current_out`/`_more_out`와 DEALER LB의 multipart pipe가
   동일 record의 continuation을 소유한다.
4. 후속 part가 HWM 등으로 실패하면 기존
   `socket_send_complete.cpp:302-308`의 `rollback_scoped`를 사용해 이미 쓴
   part를 rollback하고 terminal completion만 발생시킨다. 따라서 partial
   admission은 없다.

sync send 경로와 completion exactly-once/lifecycle 처리는 변경하지 않았다.
새 thread도 추가하지 않았다.

## 테스트 및 결과

추가한 `test_send_async_multipart`는 다음을 검증한다.

- ROUTER exact-target 2-part 및 3-part: admission, completion, 전체 전달
- 연결 후 DEALER generic target(`NULL`) 3-part: admission, completion, 전체 전달
- HWM에서 후속 part 실패 시 terminal completion과 no-partial-delivery
- 다른 target에서 sync multipart sequence가 진행 중일 때 pending async
  multipart가 per-handle gate를 기다리고 `EINVAL`로 유실되지 않는 interleaving

실행 결과:

- `test_send_async_multipart`: 통과
- `ctest --test-dir core/build-sendfix -R '^test_send_async_multipart$' --repeat until-fail:10 --output-on-failure`: 10/10 통과
- 기존 관련 inproc 테스트 `test_public_inproc_multipart_send`: 통과
- 전체 build `cmake --build core/build-sendfix -j2`: 통과
- 전체 ctest `ctest --test-dir core/build-sendfix --output-on-failure -j4`:
  92개 중 38개 통과, 54개 실패

전체 ctest 실패 비교:

- 알려져 있던 실패군이 그대로 재현됐다: `test_ctx_destroy`,
  `test_multi_socket_contract_regressions`, `test_zmp_request_reply`,
  `test_routed_submit_target`, `test_stream_threadsafe_socket_runtime_reads`,
  `test_router_concurrent_routed_recv`, `test_router_mandatory_hwm`,
  flow-state 계열의 `-j4` timeout/실패, `test_stream_send_blocking_wakeup`.
- 추가로 실행 환경의 network sandbox가 TCP/IPC/IPv6/WS open을
  `EPERM (Operation not permitted)`으로 거부해 해당 transport 테스트가
  실패하거나 timeout했다. 이는 변경된 send-complete 경로의 assertion이나
  multipart 결과와 무관하다.
- 현재 worktree의 다른 작업으로 core 문서가 이동/미생성된 상태여서
  `test_thread_safe_contract_policy`의 문서 파일 open 실패도 발생했다.
- `unittest_poller`는 단독 재실행에서도 기존 `Expected 0 Was 1` 실패가
  재현됐다. 변경된 send-complete 경로와 무관하다.
- 변경 경로의 새 테스트 및 inproc multipart 경로에는 새 실패가 없었다.

추가 확인:

```text
rg -n "send_ready" core/src core/include core/tests
=> no send_ready in core/src core/include core/tests
```

## 변경 파일

- `core/src/runtime/sockets/common/socket_send_complete.cpp`
- `core/tests/CMakeLists.txt`
- `core/tests/integration/test_send_async_multipart.cpp`
- `doc/perf/perf/bindings-0.12.0/log/2026-08-24-core-send-async-multipart-fix.md`
