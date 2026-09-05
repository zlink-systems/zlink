# Core boundary 검증 요약

판정: **BLOCKED — D-B109 전체 계약 충족 아님.** 확인된 Core 결함은 수정했으나, REJECT로 닫힌 새 연결에 admit된 REQUEST가 `NOT_CONNECTED` 대신 timeout되는 문제가 남는다. 문서 충돌을 임의의 런타임 정책으로 보상하지 않았다. 최종 integration 120/121, hotpath_gate 제외 전체 Core 170/171 통과. 남은 실패는 신규 boundary 하나다. 최종 종료 코드 `EXIT:8`.

작업 기준: detached `ba134a7aa1`; branch/commit/push/reset/checkout/stash 없음. `core/build`·`core/build-dev` 사용 없음. 빌드는 `core/build-boundary`, RelWithDebInfo/LTO OFF/tests ON, `-j3`. 테스트는 공개 C API와 기존 testutil 인증서 helper를 사용한다. spec·binding·Framework·plan 문서는 변경하지 않았다.

## 결과 분포

각 셀 20회. HANDOVER는 최종 5회 반복 중 마지막 회, REJECT는 최종 개별 셀 실행 결과다. 즉시 OK/BP는 최초 B 제출 결과이며 BP 뒤에는 공개 WRITABLE completion 계약에 따라서만 재제출한다. 모든 B timeout 인자는 1000ms다.

| Transport | RID 정책 | 횟수 | 즉시 OK | BP | B reply OK | B NOT_CONNECTED | B timeout | 새 ID DISCONNECTED 수 | C NOT_FOUND | 새 READY |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| tcp | HANDOVER | 20 | 0 | 20 | 20 | 0 | 0 | 0 | 20 | 20 |
| tcp | REJECT | 20 | 0 | 20 | 1 | 0 | 19 | 19 | 20 | 20 |
| ipc | HANDOVER | 20 | 0 | 20 | 20 | 0 | 0 | 0 | 20 | 20 |
| ipc | REJECT | 20 | 0 | 20 | 1 | 0 | 19 | 19 | 20 | 20 |
| inproc | HANDOVER | 20 | 20 | 0 | 20 | 0 | 0 | 0 | 20 | 20 |
| inproc | REJECT | 20 | 15 | 5 | 5 | 0 | 15 | 20 | 20 | 20 |
| ws | HANDOVER | 20 | 0 | 20 | 20 | 0 | 0 | 0 | 20 | 20 |
| ws | REJECT | 20 | 0 | 20 | 0 | 0 | 20 | 20 | 20 | 20 |

B가 완료될 때까지 monitor record를 소비하지 않으며 테스트에 `zlink_poll`/poller 호출이 없다. 완료 관찰에는 public nonblocking receive/completion API를 사용한다. C는 old 연결에 실제 도착시킨 뒤 reply 없이 disconnect한다. 새 READY와 새 ID DISCONNECTED는 B/C 결과 관찰 후 읽는다. Public completion은 physical ID를 반환하지 않으므로 개별 REQUEST와 physical connection의 직접 매핑을 주장하지 않는다. network의 최초 B는 모두 BACKPRESSURED였고, old ID와 다른 연결의 종료 및 timeout을 함께 관찰했다. 소스에서도 `core/src/runtime/core/pipe.cpp:3066`의 terminate가 lifecycle을 비활성화하고 `core/src/runtime/sockets/dealer/dealer.cpp:42`의 active_submit_candidate가 그 pipe를 제외하는 것을 확인했다.

## 수정과 원인

| 분류 | 소유 계층·원인 file:line | 수정 |
|---|---|---|
| B | Core WS/TLS connecter: `core/src/runtime/transports/ws/asio_ws_connecter.cpp:260,331,360,443`, `core/src/runtime/transports/tls/asio_tls_connecter.cpp:254,328,368,416` | event/engine별 ID 생성 대신 TCP/IPC와 같은 `_attempt_endpoint_pair`를 DELAYED/RETRIED/engine/CLOSED에 재사용. |
| B | Core endpoint removal: `core/src/runtime/sockets/common/socket_base_endpoint.cpp:1011`; 기존 종결 함수 `core/src/api/socket/socket_request_reply_dispatch.cpp:423` | 명시적 endpoint 제거에서 기존 pending REQUEST 종결 함수를 호출. 수정 전 공개 repro의 C=TIMED_OUT(101), 수정 후 NOT_FOUND(102). |
| B | Core ROUTER admission: `core/src/runtime/sockets/router/router_admission.cpp:361` | 중복 RID 거부가 false만 반환해 새 pipe를 anonymous 상태에 방치하던 문제. 기존 deferred termination action으로 거부 pipe를 종료. |
| B | Core inproc command ownership: `core/src/runtime/sockets/common/socket_base_endpoint.cpp:431,979`; `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:1030,1118,1184` | 같은 I/O executor의 peer 시작·종료를 동기 대기하던 경로 제거. command-only bootstrap은 mailbox 설치로 ownership을 확정하고 callback의 stop은 기존 idle detach 경로로 처리한다. completion/monitor owner 획득 경로는 유지했다. inproc의 기존 임시 owner는 attached pipe의 종료 handshake가 끝날 때까지 idle detach하지 않아 late ack와 reconnect를 처리한다. 정상 pipe만 남으면 원래대로 idle detach한다. 새 lease 상태는 추가하지 않았다. |

inproc trace는 peer start 1001ms + peer stop 10000ms 대기를 기록했다(`core-boundary-inproc-wait-trace.log`). 임시 로깅은 모두 제거했다. 기존 `test_socket_disconnect_progress_without_app_poll`은 최종 수정 후 5회 연속 통과했다.

비교한 대안: event마다 ID를 만들고 사후 매핑하는 방법 대신 connecter의 기존 attempt owner를 사용했다. inproc에서 호출부별 wait 예외나 새로운 pipe별 lease를 두는 방법 대신, 이미 있는 pipe lifecycle 상태로 실제 미완료 종료를 판단하고 command owner의 idle detach를 그 경계에 맞췄다.

수정 전/후 규칙 수: WS/TLS ID 생성 위치 4→1; command-only owner 획득 완료 조건(설치+첫 callback) 2→1; inproc owner 종료의 별도 synchronous wait 경로 1→0(기존 idle detach로 통일); REJECT anonymous 대기 예외 1→0. 새 timer·retry·generation·매핑표 없음.

## 계약 근거

- `core/doc/spec/core/socket/README.ko.md:867-873` — 성공한 disconnect의 local 등록/intent 제거, old 연결 재admission 금지, monitor/terminal edge와 독립적인 새 연결 진행, 명시적 제거 NOT_FOUND.
- 같은 문서 `:159-171` (§4) — REJECT는 새 중복 pipe를 즉시 닫고, 이미 admit된 REQUEST를 NOT_CONNECTED/EHOSTUNREACH로 정확히 한 번 종결.
- 같은 문서 `:1145` — 명시적 endpoint/RID 제거의 REQUEST NOT_FOUND.
- `core/doc/spec/core/06-monitoring.ko.md:69` (§3.1), `:539-543` — attempt identity, CLOSED의 OS handle 소유권, 성립한 연결의 DISCONNECTED, inproc CLOSED 부재.
- `core/doc/spec/core/05-polling.ko.md` §3 및 socket disconnect 절의 command progress 참조.

교차언어 대조: Framework runtime을 변경하지 않았다. 공통 Core C API에서 재현·수정했고 binding별 동작을 추가하지 않았다. transport 대조는 기존 TCP/IPC attempt 구현과 WS/TLS 공개 API lifecycle 테스트로 수행했다.

## 테스트와 게이트

최종 로그: `core-boundary-verified-identity-five.log`, `core-boundary-verified-<transport>_<policy>[-five].log`, `core-boundary-idle-owner-tests.log`, `core-boundary-verified-integration.log`, `core-boundary-verified-all.log`. 모두 이 요약과 같은 디렉터리에 있다.

| 항목 | 결과 |
|---|---|
| dev 옵션 configure / 전체 build `-j3` | PASS |
| `test_monitor_connection_identity` (TCP/IPC/inproc/WS/TLS, 19 Unity cases, inproc CLOSED 부재 포함) | 최종 5/5 PASS, ignored 0 |
| `test_socket_disconnect_boundary` HANDOVER 4 transport | 각 5/5 PASS (각 100 samples) |
| `test_socket_disconnect_boundary` REJECT 4 transport | FAIL — 표의 timeout; 새 READY/C NOT_FOUND는 20/20 |
| `test_socket_disconnect_progress_without_app_poll` | 최종 runtime 수정 후 5/5 PASS |
| `test_zmp_request_reply_receive_transaction` | 최종 runtime 수정 후 5/5 PASS; 위 progress와 합계 61.03s |
| 신규/변경 테스트 전체 5회 green | 미충족 — REJECT contract blocker |
| 최종 `ctest -L integration -j2` | 120/121 PASS; boundary만 TIMEOUT(60s), 총 254.83s |
| 최종 `ctest -j2 -E hotpath_gate` | 170/171 PASS; boundary만 TIMEOUT(60s), 총 290.07s |
| `git diff --check` | PASS (종료 직전 exit 0; 신규 파일도 별도 whitespace 검사) |

신규 boundary CTest 속성: `integration;serial`, `RUN_SERIAL=TRUE`, `TIMEOUT=60`. 최종 integration에서 기존 테스트 120개는 모두 통과했다. 전체 boundary는 실패한 REQUEST들이 각각 1000ms budget을 소진하므로 합산 60s CTest 상한에 도달한다. 8셀 전체 분포는 개별 실행으로 확보했고 TIMEOUT은 늘리지 않았다. 측정성 실행/게이트 시작은 load1 ≤3에서 수행했다. 진행 로그는 세 구간에서 3분 append 주기를 놓쳤다(233s, 208s, 457s); 원본 timestamp를 유지했으며 기록을 소급 생성하지 않았다.

## BLOCKERS / spec gap

**Spec gap: YES.** `socket/README.ko.md:873`은 일시적 physical 단절에 기존 correlation/timeout budget 유지를 요구하지만, 같은 문서 `:1149` completion 표는 transient/HANDOVER/REJECT 원인 불문 pair 종료 즉시 NOT_CONNECTED를 요구한다. 동일 REQUEST의 transient 단절에 두 결과를 동시에 구현할 수 없다.

REJECT 종료 후 REQUEST가 남는 현 경로는 `core/src/runtime/core/session_base.cpp:620-681`의 connection_error/reconnect다. 이 경로는 transport connection ID를 지우고 재연결하며 해당 REQUEST를 NOT_CONNECTED로 종결하지 않는다. `core/src/api/socket/socket_request_reply_dispatch.cpp:455`의 기존 pair 종결 함수는 현재 ROUTER handover에서 호출된다. network peer의 RID 거부를 일반 transport 단절과 구분하는 명시적 전달 계약은 확인되지 않았다(`core/src/runtime/protocol/zmp_protocol.hpp:57-73`의 ERROR code 목록에는 RID duplicate reject 구분이 없음).

필요한 결정: transient도 즉시 NOT_CONNECTED로 끝낼지, timeout 유지 계약을 보존하면서 REJECT 원인을 어떻게 전달/식별할지 spec owner가 확정해야 한다. 모든 단절을 일괄 실패시키거나 caller 재시도·timeout 증가·monitor 상태 보상으로 우회하지 않았다. 이 결정 전에는 REJECT gate를 green으로 판정할 수 없다.

변경 파일: `core/src/runtime/sockets/common/{socket_base_endpoint.cpp,socket_base_lifecycle.cpp}`, `core/src/runtime/sockets/router/router_admission.cpp`, `core/src/runtime/transports/{ws/asio_ws_connecter.cpp,ws/asio_ws_connecter.hpp,tls/asio_tls_connecter.cpp,tls/asio_tls_connecter.hpp}`, `core/tests/CMakeLists.txt`, `core/tests/integration/monitoring/test_monitor_connection_identity.cpp`, `core/tests/integration/test_socket_disconnect_boundary.cpp`.

A용 한 줄: D-B109 Core identity·명시 제거 NOT_FOUND·inproc command progress·REJECT pipe 종료 수정(B); REJECT REQUEST terminal은 transient 계약 충돌(spec gap)로 BLOCKED, 전체 green 아님.

종료 시각: 2026-09-05T13:27:34+09:00. 작업 시간 약 68.0분(90분 상한 이내).
