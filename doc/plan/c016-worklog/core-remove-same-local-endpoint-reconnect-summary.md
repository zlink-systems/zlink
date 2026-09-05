# D-094 ROUTER 동일 endpoint 재연결 정책 수정 결과

D-094의 admission 예외를 제거하고, 종료된 pipe가 RID를 보유한 채 재시도를 거부하는
종료 순서와 TCP 주소 별칭의 종료 귀속을 수정했다. **최종 승인 보류:** 새 테스트의
TCP 자동 재연결에서 같은 RID의 미완성 lane 결합 충돌이 남는다. 실패 단언은 유지했다.

작업 기준은 `/home/hep7/project/zlink-core-b`, detached `b4a0cf8421`이다.
요청한 reset·core clean·checkout을 수행했다. main과 worktree a의 빌드에는 접근하지
않았으며, spec·bindings·framework 수정과 commit은 없다. main 변경은 이 보고서뿐이다.

## 원인과 수정

### Admission 정책

기준 커밋의 `core/src/runtime/sockets/router/router_admission.cpp:345-360`은 같은
locally initiated endpoint이면 REJECT 검사를 우회했다. 상대 ROUTER는 새 pipe를
REJECT하지만 connector는 기존 정상 pipe를 교체하고 종료하므로 양쪽 연결을 잃었다.
공개 C API 원재현과 Java 관찰 순서는
[`fix-java-descriptor-fence-class-run-summary.md`](./fix-java-descriptor-fence-class-run-summary.md)의
“Core 원인과 공개 C API 시퀀스”를 기준으로 했다.

수정본 `router_admission.cpp:343-356`은 동일 endpoint 여부를 검사하지 않는다.
REJECT는 새 중복 pipe를 종료하고, HANDOVER는 기존 인수 경로를 사용한다.
`paired_application && reciprocal_duplicate` 분기와 RID 방향 비교는 변경하지 않았다.

### 종료 순서와 RID 해제

예외 제거만 적용한 inproc 순서 테스트는 반복 중 실패했다. 새 attempt가 잠시 RID를
받았다가 거부될 때, 미수신 routing-id preamble이 final ack를 지연시켰다.
`socket_base_api.cpp`의 기존 `pipe_peer_terminated()`는 D-092 DISCONNECTED와
재연결 command를 게시하지만, RID 제거는 `pipe_terminated()` 이후의
`router_t::xsocket_msg_pipe_terminated()`까지 지연됐다.

기존 `ZLINK_DEBUG_PIPE_TERM=1 ZLINK_ROUTER_DEBUG=1` trace에서 동일 pipe의
`process_pipe_term` 이후에도 같은 RID로 write를 시도하다 실패하고, 후속 attempt가
계속 거부되는 것을 확인했다. client의 application receive 없이 3초 안에 request가
완료돼야 하는 단언이 실패했다. 로그는 `/tmp/zlink-core-d094-order-owner.log`에 있다.

`pipe.cpp:3007-3012`는 기존 `ack_peer` 상태로 local-close 순서에도 논리 종료를
한 번 통지한다. peer-close 순서는 기존 `process_pipe_term()`이 통지한다.
`socket_base_api.cpp:1720`은 그 통지에서 accepted-pair 연결 정보를 해제하고,
기존 deferred route teardown을 등록한다. receive lock 밖에서 정리하는 기존 경로를
유지하면서 RID 제거 시점을 final drain 이전으로 옮겼다. final release의 중복 통지와
route teardown은 제거했다. 새 상태·타이머·retry 횟수·admission 조건은 추가하지 않았다.

### TCP 주소 별칭의 종료 귀속

TCP는 같은 endpoint 문자열의 두 번째 `connect()`를
`socket_base_endpoint.cpp:480`에서 성공한 무동작으로 처리한다. 테스트는 이 호출 뒤
request/reply를 확인한 다음, 같은 listener의 `127.0.0.1`/`localhost` 주소로 별도
physical connection을 만들어 중복 정책을 검사한다. inproc는 같은 문자열을 그대로 사용한다.

이 TCP 검증에서 RECONNECT_IVL=-1인 새 session의 종료가 기존 연결까지 종료했다.
`session_base.cpp`의 resolved-address 기반 `term_endpoint` 생성과, 양쪽 lane에서 온
중복 종료 command가 사라진 주소 별칭을 다른 intent로 다시 resolve하는 것이 원인이었다.
내부 종료 함수의 `process_commands()` 재진입도 중복 명령의 처리를 겹치게 했다.

`session_base.cpp:741`의 기존 `process_conn_failed()`가 원본 connect 주소로 명령을
만들도록 하고, 재연결 종료 경로도 이 함수를 사용한다. 내부 command는 정확한 endpoint
등록이 있을 때만 종료한다(`socket_base_lifecycle.cpp:1393`). 함수 진입 시 command drain은 공개
`term_endpoint()` 경계로 옮겼다(`socket_base_endpoint.cpp:1114`). TCP REJECT에서
기존 connection id의 DISCONNECTED가 없고 양방향 request/reply가 계속됨을 확인했다.

대안은 admission이 종료 중인 old pipe를 특별 취급하거나, 종료 소유자가 같은 종료
전이에서 routing 정보를 해제하는 것이다. endpoint·generation 예외를 추가하지 않고
기존 소유자를 사용하는 후자를 적용했다.

## 계약·소유권·교차언어·분류

- **소유 계층:** Core ROUTER admission, pipe 종료 상태 전이, socket endpoint command owner.
- **Spec 조항:** Core `socket/README.ko.md` §4 RID 중복 정책(REJECT/HANDOVER), §6 pair 종료 completion, `zlink_disconnect` 경계; polling §3 command progress. D-088·D-091·D-092·D-094 및 D-B96을 적용했다.
- **교차언어 대조:** 공개 C API만으로 검사하므로 모든 binding이 사용하는 Core 경로의 수정이다. 위 Java 진단 보고서에서 Java 기본 REJECT와 C++·Node·.NET HANDOVER 설정 차이를 대조했다. Framework runtime은 수정·검증하지 않았다.
- **변경 분류:** **B 기존 결함**. 공개 API·wire 계약 변경은 없다. 남은 TCP lane 결합 문제는 아래 BLOCKERS에 분리했다.
- **수정 전/후 규칙 수:** admission **3→2**(REJECT/HANDOVER; 기존 reciprocal 처리 보존). 논리 종료 통지 소유자는 socket의 final-release 보상과 pipe 전이에서 pipe 전이 하나로 통합했다. session의 endpoint 종료 명령 생성은 3곳에서 기존 함수 1곳으로 통합했다.

## 검증

빌드: `nice -n 10 env JOBS=4 scripts/build-core.sh dev` 성공.
새 테스트는 `core/tests/integration/test_router_same_socket_reconnect_policy.cpp`이며
`core/tests/CMakeLists.txt`에 등록했다. sleep·내부 심볼·failpoint를 사용하지 않는다.

| 검증 | 결과 |
|---|---|
| 새 테스트 전체 `--repeat until-fail:10` | 첫 회 통과, 두 번째 회 TCP retry의 HANDSHAKE_FAILED_PROTOCOL 단언 실패 |
| inproc REJECT 단독 `--repeat until-fail:10` | 10회 통과 |
| inproc ordering/retry 단독 `--repeat until-fail:10` | 10회 통과 |
| 기존 표적 gate 7개 | 7/7 통과, 46.95초 |
| 전체 ctest 1회, `-j2 -E '^hotpath_gate$'` | **176/176 통과**, 234.43초 |
| 원래 공개 C API 재현(이번 worktree shared library) | REJECT/HANDOVER 모두 exit 0; 양쪽 `original_closed=0` |
| `git diff --check` | 통과 |
| hotpath | 요청에 따라 n/a; 연결·종료 경로 변경 |

표적 gate는 `test_router_reject_duplicate`,
`test_router_reject_disconnected_without_app_recv`, `test_socket_disconnect_boundary`,
`test_router_reciprocal_handover_lanes`, `test_ctx_term_fixed_rid_handover`,
`test_router_handover`, `test_inproc_pending_connect_rejected_at_attach`다.

REJECT 검사는 두 번째 connect 뒤 monitor만 소비하여 새 pipe의 DISCONNECTED를
확인하고, 기존 connection id가 유지되며 양방향 request/reply가 계속됨을 검사한다.
HANDOVER 검사는 새 READY와 기존 양쪽 pending request의 NOT_CONNECTED completion,
새 request/reply, 기존 connection의 DISCONNECTED 부재를 검사한다. 구현은 이전
pipe를 standby로 등록한다(`router_admission.cpp:406-413`).

순서 검사는 old RID가 확실히 존재하는 동안 새 attempt의 거부를 관찰한 다음
`zlink_disconnect_rid()`로 기존 peer의 비동기 종료를 요청한다. 이는 old terminate
처리 이전의 중복을 공개 API로 고정한 것이다. 내부 command 큐를 정지시키지는 않는다.
그 뒤 추가 `connect()` 없이 Core connect intent가 재시도한다. READY는 RID admission
보장이 아니므로, 전환 중 REQUEST_NOT_CONNECTED는 §6에 따라 caller가 재제출한다.
client application receive 없이 제한 시간 안의 성공 completion을 요구하여, recv가
old preamble을 소진해서 reconnect를 진행시키는 우회를 막는다.

## 변경 파일과 산출물

- `core/src/runtime/sockets/router/router_admission.cpp`
- `core/src/runtime/core/pipe.cpp`
- `core/src/runtime/core/session_base.cpp`
- `core/src/runtime/sockets/common/socket_base_api.cpp`
- `core/src/runtime/sockets/common/socket_base_endpoint.cpp`
- `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`
- `core/tests/integration/test_router_same_socket_reconnect_policy.cpp`
- `core/tests/CMakeLists.txt`

패치: `/home/hep7/project/zlink-core-b/core-d094.patch`.
기존 untracked patch와 `diag-node-old-core/`는 임시 `core.excludesFile`을 사용해
`git add -N .` 대상에서 제외한 뒤 `git diff HEAD`로 패치를 생성했다.
패치 SHA-256: `c24a9bb017f5d81b794f3a70377339f8f0847802dc5ae0274d868a57a4d24373`.
`git apply --reverse --check`로 현재 변경과 일치함을 확인했다.
보고서는 문서 원칙·코드 부합의 독립 2축 검토를 완료하고, 문구 범위 지적 2건을 반영했다.
로그: `/tmp/zlink-core-d094-{repeat-final,targeted-final,full}.log` 및
`/tmp/zlink-core-d094-{reject-inproc-repeat,order-owner-repeat}.log`.

## BLOCKERS

1. **TCP의 동시 connect intent 재시도에서 count-two lane 결합 충돌.**
   `socket_base_api.cpp:84-100`의 `adopt_accepted_transport_pair()`는 같은 RID의
   미완성 pair를 재사용한다. `attach_pipe():338-342`에서 기존 Completion lane과
   다른 Completion lane이 같은 pair에 들어오면 `:447`에서
   `ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_READY`를 게시한다.
   `/tmp/zlink-core-d094-gdb-pair.log`는 서버에서 `completion=true`,
   `reject_pipes={new completion, NULL, existing completion}`를 확인했다.
   재현은 `ZLINK_TEST_CASE=test_reject_retry_tcp`로 실행하는 새 공개 C API 테스트다.
   종료된 association을 더 일찍 제거해도 동시에 진행되는 미완성 attempt의 충돌은
   남는다. pair 결합 소유자의 후속 진단이 필요하며, 본 패치는 이 실패를 허용하도록
   assertion을 바꾸지 않았다. 따라서 새 테스트 10회 gate는 green이 아니다.
전체 ctest 1회에서는 새 테스트까지 통과했지만, 반복 gate의 TCP 실패를 해소한 것으로
판정하지 않는다. 원재현 로그는 `/tmp/zlink-core-d094-c-api-{reject,handover}.log`다.
