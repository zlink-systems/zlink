# Core: zlink_close가 bound inproc endpoint를 반환 전에 해제 — 요약

판정: **FIXED (분류 B, 기존 결함).** `zlink_close()`가 `ZLINK_CLOSE_OK`를 반환한 뒤에도 bound inproc endpoint 등록이 reaper thread의 `process_term`까지 살아 있어, 같은 context의 다른 socket이 같은 `inproc://…`를 바로 bind하면 `EADDRINUSE`가 났다. 등록 해제를 close의 caller-thread 완료 단계(`finish_close_reap`, `send_reap` 이전)로 옮겼다. 새 registry·bind-side retry/wait·옵션 없음.

작업 기준: worktree `/home/hep7/project/zlink-core-a`, detached `e596c0e7cd` (main HEAD `4a76f8b489`와 `core/` 차이 없음). commit 없음. 빌드 `scripts/build-core.sh dev` (worktree의 `core/build-dev`; main tree build-dev 미사용). spec·binding·framework 문서 변경 없음. 패치: `/home/hep7/project/zlink-core-a/core-close-inproc.patch`.

## 원인 (file:line)

- `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:1361-1371` (`socket_base_t::process_term`) — inproc endpoint 해제 `unregister_endpoints (this)`가 여기 있었다. `process_term`은 reaper thread의 `start_reaping()` → `terminate()`(`socket_base_lifecycle.cpp:354`)에서만 호출되며, 그 시점은 `zlink_close`가 반환한 뒤다.
- `core/src/api/core/zlink.cpp:113` `zlink_close` → `complete_close_handoff` → `socket_base_api.cpp:164 finish_close_handoff` → `:172 finish_close_reap` → `send_reap` → 반환. 즉 caller가 관찰하는 close 완료와 endpoint 해제 사이에 reaper 스케줄링 지연이 있었고, 그 창에서 새 socket의 `register_endpoint` (`core/src/runtime/core/ctx_inproc_registry.cpp:41-53`)가 `EADDRINUSE`를 반환했다.
- 반면 `zlink_unbind`는 `socket_base_endpoint.cpp:1034 term_endpoint_internal`에서 caller thread가 `unregister_endpoint`를 동기적으로 호출한다. 같은 사실(bound endpoint 해제)에 두 가지 시점 규칙이 있었다.

## 수정

- `core/src/runtime/sockets/common/socket_base_api.cpp:172-187` `finish_close_reap`: completion 종료·blocking send 실패 처리 뒤, `materialize_pending_inprocs_before_reap ()` 앞에 `unregister_endpoints (this)`를 둔다. 이 함수는 `send_reap` 직전 caller thread에서 실행되며 async 명령 처리가 quiesce된 뒤다. 새 inproc connect가 teardown 중인 socket에 도달하는 창도 같이 닫힌다.
- `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:1361` `process_term`: `unregister_endpoints` 호출 제거(주석으로 소유 위치 명시). `disable_transport_pair_reconnects`·pipe 종료는 그대로.
- 테스트 `core/tests/integration/test_close_releases_inproc_endpoint.cpp` 신규, `core/tests/CMakeLists.txt`에 등록(integration;serial). 4 case: (1) close 직후 rebind ×300 (connect 진행 중 close), (2) CONNECTION_READY edge 관찰 후 close → rebind ×50 (Java M6A 형태), (3) unbind→close→rebind ×10, (4) tcp loopback 동일 계약 ×30. 공개 C API만 사용, sleep 기반 타이밍 추정 없음(rebind는 close 반환 직후 1회 호출로 판정).

소유 계층: Core socket lifecycle (`socket_base_t` close 경로) + ctx inproc registry(기존 소유자, 변경 없음).
spec 조항: `core/doc/spec/core/socket/README.ko.md` § zlink_close(~605-622) "socket을 닫고 관련된 모든 자원을 해제한다"; § zlink_bind(~795-818) "주소가 이미 사용 중이면 `EADDRINUSE`"; § zlink_unbind(~843) binding 제거.
교차언어/transport 대조: Framework 변경 없음. tcp/ipc listener는 unbind와 close 모두 `term_child` → listener `process_term`(I/O thread)에서 OS listen socket을 닫는다(두 경로 같은 규칙). inproc도 이제 unbind와 close가 같은 규칙(caller thread, 반환 전 registry 해제)이다.
변경 분류: **B** (기존 결함).

수정 전/후 규칙 수: bound inproc endpoint 해제 시점 규칙 2(unbind=caller thread 동기 / close=reaper 비동기) → 1(bind를 끝내는 공개 호출이 반환 전에 해제). 새 상태·타이머·헬퍼·registry 0.

## 결과

| 항목 | 결과 |
|---|---|
| 공개 repro `/tmp/zlink-java-m6a-intermittent/close_rebind_inproc.cpp` 형태(확장판, CPU busy-loop 4개 병행) 수정 전 | inproc close-only 3000회 중 `EADDRINUSE` **2991** |
| 같은 repro 수정 후 | inproc close-only 3000회 `EADDRINUSE` **0**; unbind-then-close 20회 0; tcp close-only 300회 0 |
| 새 테스트, 수정 전(fix stash) 부하 하 | FAIL: case(1) iteration 1, case(2) iteration 23 `Address already in use (98)`; case(3)(4) PASS |
| 새 테스트 `--repeat until-fail:10`, CPU busy-loop 4개 병행 | 10/10 PASS (각 ~3.0s, 총 30.35s) |
| 새 테스트 `--repeat until-fail:10`, 무부하 | 10/10 PASS (총 30.19s) |
| 지정 ctest `-R 'test_inproc|test_ctx|test_router_reject_duplicate|test_router_same_socket_reconnect_policy|test_inproc_pending_connect_rejected_at_attach|test_multi_socket_contract_regressions'` | 8/8 PASS (7.27s) |
| 전체 `ctest --test-dir core/build-dev -j2 -E '^hotpath_gate$'` | 179/179 PASS (240.07s; 새 테스트 포함, hotpath_gate 제외) |
| `git diff --check` | clean |

hotpath_gate: 제외 실행(요청 범위). 성능 hot path 변경 없음(close 경로만).

## 관찰 사항 / BLOCKERS

- **BLOCKER 없음** (close 계약 기준).
- 별도 결함(미수정, unbind 지연): `zlink_unbind(inproc)`이 ROUTER↔ROUTER처럼 lane 2개인 pair에서 호출당 약 200ms 걸린다. `socket_base_endpoint.cpp:969` `terminate_inproc_pipe_with_peer_progress`가 `get_transport_lane_count () == 1u`일 때만 peer의 async command 처리를 시작하므로, 2-lane pair에서는 peer가 termination ack를 보내지 못하고 `:1053-1061`의 20×10ms `process_commands` 대기가 전부 소진된다(gdb stack으로 확인). 기능은 정확(endpoint는 동기 해제, rebind 성공)하나 지연이 크다. 새 테스트의 unbind case를 10회로 제한한 이유. 별도 job 권고.
- tcp 관찰: `asio_tcp_listener.cpp:83`이 acceptor에 `SO_REUSEPORT`를 켠다. 그래서 close 직후 같은 port rebind는 listener의 OS fd가 I/O thread에서 아직 닫히기 전에도 성공한다(두 listener 공존). tcp의 "close 반환 전 OS 자원 해제"는 지금도 비동기지만 공개 동작으로는 관찰되지 않는다. spec에 명시적 문장이 없어 D(spec gap) 후보로만 기록.
- `-R 'test_inproc…'` 패턴은 새 테스트 이름(`test_close_releases_inproc_endpoint`)과 매치되지 않아 별도 실행했다.
