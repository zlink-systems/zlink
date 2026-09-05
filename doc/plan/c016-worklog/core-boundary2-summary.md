# Core boundary pass 2 검증 요약

판정: **사용자가 지정한 spec-gap 처리 경로 완료, 모든 실행 게이트 PASS(EXIT:0)**. **REJECT REQUEST 종결 계약은 spec gap으로 BLOCKED**. 감독자 결정대로 정책 거부에는 socket §4의 NOT_CONNECTED가 우선하고, 일시적 physical 단절은 기존 timeout budget을 유지한다. 남은 문제는 우선순위가 아니라 거부 사유 전달 계약의 부재다.

작업 기준: detached `ba134a7aa1`, 앞 job 미커밋 변경 보존. 이번 pass의 저장소 수정은 `core/tests/integration/test_socket_disconnect_boundary.cpp:145-153`의 REJECT `TEST_IGNORE`뿐이다. Runtime·spec·Framework·binding·doc/plan 수정 없음. stash/checkout/reset/branch/commit/push 없음. 빌드는 실제 디렉터리 `core/build-boundary`, `-j3`; `core/build`·`core/build-dev` symlink 사용 없음.

## 확인 결과와 원인

| 경계 | 결과 | 근거 file:line |
|---|---|---|
| ROUTER REJECT 송신 | 이유 없는 pipe 종료. ZMP ERROR 송신 없음 | `core/src/runtime/sockets/router/router_admission.cpp:361-367,466-469` |
| ZMP control·code | HELLO/READY/ERROR만 있으며 duplicate-reject 코드 및 별도 CLOSE 없음 | `core/src/runtime/protocol/zmp_protocol.hpp:57-73` |
| ERROR 수신 | 코드 저장 뒤 EPROTO; 정책 거부로 분류하는 경로 없음 | `core/src/runtime/engine/asio/asio_zmp_engine.cpp:705-712` |
| DISCONNECTED value | UNKNOWN(0), HANDSHAKE_FAILED(3), TRANSPORT_ERROR(4), CTX_TERM(5); duplicate-reject 없음 | `core/include/zlink_enum.h:194-200`; event 구조 `core/include/zlink/eventing/api.h:39-41` |
| Engine→session | protocol/connection/timeout만 전달. Monitor reason은 local 관찰값 | `core/src/runtime/engine/i_engine.hpp:17-22`; `core/src/runtime/engine/asio/asio_engine.cpp:1926-1946` |
| Connector reconnect | 일반 close는 connection_error로 reconnect, 기존 pending request timeout 유지. protocol_error는 reconnect 비활성화 | `core/src/runtime/core/session_base.cpp:620-681` |
| inproc 종료 | pipe_term payload가 비어 있어 정책 거부를 구분하지 못함 | `core/src/runtime/core/command.hpp:166-170`; `core/src/runtime/core/object.cpp:428-433`; `core/src/runtime/core/pipe.cpp:3066,3085-3087` |
| 기존 NOT_CONNECTED 종결 소유자 | pair/generation으로 pending을 찾아 EHOSTUNREACH로 종결; ROUTER handover에서 호출 | `core/src/api/socket/socket_request_reply_dispatch.cpp:455-469`; `core/src/runtime/sockets/router/router_admission.cpp:456-464` |

ERROR reason 문자열도 해결 수단이 아니다. `core/src/runtime/protocol/zmp_control.hpp:387-415`는 code를 반환하고 reason bytes는 길이 검증에만 사용한다. ERROR 송신은 `asio_zmp_engine.cpp:170-203,221-224`의 protocol/timeout 경로이며, ROUTER admission의 정책 거부와 연결되어 있지 않다.

## Spec 조항

- `core/doc/spec/core/socket/README.ko.md:159-171` §4: REJECT 새 중복 pipe 즉시 종료, admit된 request는 NOT_CONNECTED/EHOSTUNREACH로 정확히 한 번 종결, connect intent에 따라 재시도 및 기존 pipe 종료 뒤 admission. READY는 peer RID admission을 보장하지 않는다.
- 같은 문서 `:867-873`: 명시 제거 NOT_FOUND, 일시적 physical 단절은 replay 없이 correlation·timeout budget 유지.
- `core/doc/spec/core/protocol/01-zmp.ko.md:170-174` 및 `:504`: ERROR `[type:0x05][code:u8][reason length:u8][reason bytes]` 형식. duplicate-reject code·처리 의미는 정의되어 있지 않다.
- 앞 job이 지적한 completion 표 `socket/README.ko.md:1149`의 포괄 표현은 이번 감독자 결정에 따라 정책 거부와 transient를 구분하여 해석했다. 이를 이유로 모든 단절을 즉시 종결하지 않는다.

## 수정

REJECT tcp/ipc/inproc/ws 셀 시작에 spec gap 사유와 현 timeout 증상을 명시한 `TEST_IGNORE_MESSAGE`를 넣었다. 기존 NOT_CONNECTED 기대 assertion은 유지했다. REJECT 셀의 본문은 실행하지 않으며 timeout을 성공으로 인정하지 않는다. HANDOVER 및 C=NOT_FOUND/new READY 검증은 그대로 실행한다. Timeout·budget·retry 횟수 및 CTest 상한 변경 없음.

소유 계층: Core ROUTER admission → protocol/engine/session의 종료 원인 전달 → 기존 request pending 종결 소유자.
분류: D(spec gap) 보고 및 사용자 지시의 테스트 제외 처리; 이번 pass에 runtime 구현 수정 없음. 앞 job의 B 수정은 보존.
교차언어 대조: Framework runtime 변경 없음. 공통 Core의 tcp/ipc/ws와 inproc 종료 전달 구조를 대조했으며 모두 REJECT 식별 정보가 없다.
수정 전/후 runtime 규칙 수: 변경 없음(추가 0, 삭제 0). 새 상태·timer·retry·mapping·종결 메커니즘 없음.

## 테스트와 게이트

| 항목 | 결과 |
|---|---|
| 기존 `core/build-boundary` 전체 build `-j3` | PASS |
| `test_monitor_connection_identity` | 5/5 PASS; 매회 19 Unity cases, ignored 0 |
| `test_socket_disconnect_boundary` | 5/5 PASS; 매회 HANDOVER 4셀 통과, REJECT 4셀 명시적 IGNORE |
| `test_socket_disconnect_progress_without_app_poll` | 5/5 PASS |
| `test_zmp_request_reply_receive_transaction` | 5/5 PASS |
| 위 관련 테스트 `ctest --repeat until-fail:5 -j2` | PASS, 77.74s |
| `ctest -L integration -j2 --output-on-failure` | 121/121 PASS, 194.28s |
| `ctest -j2 -E hotpath_gate --output-on-failure` | 171/171 PASS, 232.27s |
| `git diff --check` | PASS |
| untracked boundary 별도 whitespace 검사 | PASS |

Boundary 5회 합산 결과:

| Transport | 정책 | samples | 즉시 OK | BP | B reply OK | B NOT_CONNECTED | B timeout | C NOT_FOUND | 새 READY |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| tcp | HANDOVER | 100 | 0 | 100 | 100 | 0 | 0 | 100 | 100 |
| ipc | HANDOVER | 100 | 0 | 100 | 100 | 0 | 0 | 100 | 100 |
| inproc | HANDOVER | 100 | 100 | 0 | 100 | 0 | 0 | 100 | 100 |
| ws | HANDOVER | 100 | 0 | 100 | 100 | 0 | 0 | 100 | 100 |
| tcp/ipc/inproc/ws | REJECT | 실행 제외 | — | — | — | — | — | — | — |

REJECT의 현 timeout은 앞 job `core-boundary-summary.md`의 20회/셀 관찰 결과이며 이번 pass에서 재측정하지 않았다. 4개 REJECT 셀은 `TEST_IGNORE`로 본문을 건너뛴다. CTest의 121/121·171/171은 실행 파일 단위 green이고 REJECT 계약 준수를 뜻하지 않는다. 실행 실패 0, 남은 계약 blocker 1.

로그: `core-boundary2-build.log`, `core-boundary2-five.log`, `core-boundary2-five-details.log`, `core-boundary2-integration.log`, `core-boundary2-integration-details.log`, `core-boundary2-all.log`, `core-boundary2-all-details.log`, `core-boundary2-diff-check.log`. `*-details.log`는 Unity PASS/IGNORE와 셀별 분포를 보존한다. 게이트 exit/duration은 `core-boundary2-gate-results.json`에도 기록했다.

## BLOCKERS / spec gap

**Spec gap: YES — REJECT 종료의 wire 및 session 식별 계약 부재.** §4의 결과 계약은 확정되어 있으나 connector가 일반 physical close와 구분할 정보가 없다. Monitor enum 값만 추가해도 peer→connector 사유 전달이 생기지 않으므로 해결되지 않는다.

최소 제안(미구현): 기존 ZMP ERROR에 duplicate-RID rejection code 하나를 정의하고, 그 의미를 일반 protocol failure와 구분한다. ROUTER admission이 거부를 결정하고 종료 전에 사유를 전달하며, 수신 engine/session은 해당 pair/generation의 기존 `fail_pending_requests_for_transport_pair`를 ID가 지워지기 전에 사용하여 정확히 한 번 NOT_CONNECTED/EHOSTUNREACH로 종결하고 reconnect intent는 유지한다. inproc에도 같은 의미를 기존 pipe 종료 command에 전달하도록 계약을 정해야 한다. 숫자 code 및 enum을 임의로 배정하지 않았다.

ERROR 송신의 완료·close 순서도 계약에 포함해야 한다. 현재 `send_error_frame`은 EAGAIN에서 송신을 중단하므로 best-effort frame 송신만 추가하면 정확한 REJECT 식별을 보장하지 못한다. 이는 새 retry 규칙을 도입하자는 제안이 아니라 기존 송신 소유자가 ERROR 전달과 종료를 일관되게 소유해야 한다는 요구다.

대안 비교: 기존 ERROR의 식별 code 확장은 이미 있는 framing을 재사용한다. 별도 CLOSE 명령 신설 또는 monitor enum만 추가하는 방법은 각각 protocol 규칙을 늘리거나 wire 정보 부재를 해결하지 못하므로 채택하지 않았다. Spec owner의 전달 계약 확정 후 기존 종결 함수를 재사용하는 구현이 필요하다.


A용 한 줄: Core pass2는 REJECT 식별 wire/session 계약 부재(D)를 확인하여 지정대로 REJECT 4셀 IGNORE 처리; 관련 5회·integration 121/121·전체 171/171·diff-check PASS, runtime 추가 수정 없음, REJECT NOT_CONNECTED 계약은 spec owner 결정 대기.

종료 코드: `EXIT:0` (요청된 spec-gap 대안 및 게이트 완료; REJECT 구현 완료를 뜻하지 않음).

종료 시각: 2026-09-05T13:42:10+09:00. 진행 로그 최대 간격 120.0s(3분 이내).
