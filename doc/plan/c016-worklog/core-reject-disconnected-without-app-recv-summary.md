# Core REJECT 종료의 monitor 관찰 — D-092

D-092의 inproc ROUTER 결함을 수정했다. Connector가 application recv를 호출하지 않아도
거부된 pipe의 DISCONNECTED를 관찰하고, 남아 있는 connect intent로 다시 연결한다.
신규 공개 API 테스트에서 종료 관찰과 자동 재연결을 각각 10회 확인했다.
최종 전체 ctest는 174/176 통과했다. 남은 실패는 별도 tcp ROUTER HANDOVER 결함과 dev에서
n/a인 hotpath 항목이다. 신규 전체 반복 gate는 HANDOVER 결함으로 막혀 있다.

- 작업 트리: `/home/hep7/project/zlink-core-b`, detached `b63f79a3ce` 기준.
- 패치: `/home/hep7/project/zlink-core-b/core-d092.patch`.
- main에는 이 요약만 작성했다. main의 `core/build-dev`, spec, bindings, framework는 수정하지 않았다.
- 커밋하지 않았다. 기존 untracked `core-send-routing-id.patch`, `diag-node-old-core/`는 유지하고 패치에서 제외했다.

## 원인과 소유 계층

기준 커밋의 `core/src/runtime/sockets/router/router_admission.cpp:270`은 connector pipe에 이미
알려진 RID가 있으면 이를 사용한다. Inproc의 별도 routing-id preamble은 이때 FIFO에 남는다.
REJECT는 `router_admission.cpp:461`에서 `terminate(true)`로 중복 pipe를 닫는다.
`core/src/runtime/core/pipe.cpp:2935-2945`는 preamble도 읽을 것이 남은 상태로 보고
`waiting_for_delimiter`로 전이한다. Peer 종료 콜백은 `pipe.cpp:2968`에서 이미 호출되지만,
DISCONNECTED 게시와 inproc 재연결 예약은 기준 커밋의
`core/src/runtime/sockets/common/socket_base_api.cpp:1762,1925`에 있어 final ack까지 기다렸다.
따라서 application이 preamble을 읽기 전에는 monitor와 재연결이 진행되지 않았다.

- 소유 계층: Core pipe 종료 → `socket_base_t::pipe_peer_terminated`; inproc connect intent는 `socket_inprocs_t`가 소유한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md` §4 RID 중복 정책(:159-165), §6 `zlink_disconnect`(:869-874), `core/doc/spec/core/05-polling.ko.md` §3(:73-81). D-B104·D-088·D-091·D-092 적용.
- Parity: ROUTER/DEALER 및 tcp/inproc를 같은 공개 C API 테스트로 대조했다. C++ `bindings/cpp/src/Runtime/Eventing/monitor.cpp:145`와 .NET `bindings/dotnet/src/Zlink/Runtime/Eventing/SocketMonitor.cs:17`은 모두 Core monitor recv를 호출한다. 언어별 보상이나 Framework 변경은 없다.
- 분류: **B — 기존 Core 결함**, D-092에서 승인된 계약 복원. D-092 본문은 기준 커밋에 없어 main의 `decisions.ko.md:1247`에서 읽었다.

## 수정과 수명

`socket_base_api.cpp:1717`의 기존 peer 종료 콜백으로 DISCONNECTED 게시와 inproc 재연결 예약을
옮겼다. Transport engine과 공유하던 pipe의 event claim을 그대로 사용하므로 final ack에서
같은 콜백을 호출해도 중복 event나 재연결 예약은 생기지 않는다. Inproc 등록은 close가 pending
peer를 찾는 데에도 사용하므로 final ack까지 유지한다. 재연결은 기존
`send_reconnect_inproc` command로 예약한다. 새 타이머·재시도 상태·drain 경로는 없다.

`router_admission.cpp:454`의 HANDOVER supersession은 물리적 종료가 아니다. 기존 request 소유자의
`fail_pending_requests_for_transport_pair`를 직접 호출하여, standby의 물리적 연결을 유지하면서
D-090/D-091의 request 종결을 보존한다. 물리적 종료 콜백을 논리적 교체에 재사용하지 않는다.

`socket_endpoint_runtime.cpp:117`은 explicit disconnect의 intent 등록을 먼저 제거한 뒤,
`:121`에서 기존 context helper로 pending peer를 처리한다. 기존 materialization 호출은
`socket_base_endpoint.cpp`에서 이곳으로 이동했다. 이 과정의 pipe 포인터는 lifetime reference로
유지한다. 등록을 나중에 지우면 helper가 만든 peer 종료가 취소할 intent를 다시 연결하도록
예약하여 pending-connect close 중 `_sink` assertion으로 이어진다. 이를 registry 소유 경계에서
해결했으며, 관련 기존 공개 API 테스트가 10회 통과했다.

수정 전/후 규칙 수: **종료 관찰 2 → 1** — transport error는 즉시, pipe 종료는 FIFO drain 뒤에
관찰하던 차이를 없애고, 물리적 종료가 알려지면 기존 claim으로 한 번 게시한다.
Connect intent의 판정 기준은 전후 모두 registry 등록 유무 하나이며, explicit removal은 peer
command를 진행하기 전에 등록을 제거한다.

Staged preamble의 수명은 **변경하지 않았다**. Pipe의 `waiting_for_delimiter`, drain, final ack,
inbound queue 해제 코드는 그대로다. Preamble과 미수신 DATA는 기존 recv 또는 no-delay teardown이
처리한다. Preamble을 강제로 소진하는 대안은 수신 수명을 바꾸므로 채택하지 않았다.

`socket_base_api.cpp:261`의 bind 소유권 검사는 `is_lifecycle_active()` 대신
`has_completed_termination()`을 사용한다. 종료 명령이 bind를 앞질러도 아직 final ack가 끝나지
않은 pipe는 socket이 소유해야 한다. 종료 중 socket의 ack 등록도 scheduler admission보다 앞에
옮겼다. 그렇지 않으면 monitor PAIR의 재연결에서 peer가 `waiting_for_delimiter`인 채 소유 목록에서
빠져 socket 하나와 term ack 하나가 남는다. GDB로 이 상태와 context pending registry가 비어 있음을
확인했다(`d092-ctx-pipe-gdb.log`). 기존 `test_multi_socket_contract_regressions`의 세 번째 case가
이 경계를 검출했으며, 수정 뒤 suite 전체가 10회 통과했다. 최종 해제 전까지 pipe를 소유한다는
규칙으로 바꾸었고, 별도 상태나 예외 경로를 추가하지 않았다.

## 검증 결과

신규 파일은 `core/tests/integration/test_router_reject_disconnected_without_app_recv.cpp`이며
`core/tests/CMakeLists.txt`에 integration/serial 테스트로 등록했다. Tcp/inproc × ROUTER/DEALER ×
REJECT/HANDOVER와 inproc ROUTER 자동 재연결을 검사한다. Connector에는 app recv/poll을 호출하지
않고 monitor recv와 monitor에 대한 `zlink_poll`만 사용한다. DISCONNECTED는 해당 application
connection_id로 확인하고, 종료 관찰 상한은 기존 D-B104 테스트와 같은 200ms다.
HANDOVER는 old pipe가 standby로 유지되는 계약에 따라 binder close로 실제 종료를 발생시킨다.

| 검증 | 결과 | 작업 트리의 로그 |
|---|---|---|
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS, RelWithDebInfo / LTO OFF | `d092-build-owner.log` |
| 수정 전 공개 API 재현 | inproc ROUTER DISCONNECTED 200ms 초과; DEALER 통과. Tcp ROUTER HANDOVER의 별도 READY 실패도 재현 | `d092-baseline.log`, `d092-router-trace.log` |
| 신규 테스트 `--repeat until-fail:10` | 첫 회 8/9 case PASS, tcp ROUTER HANDOVER READY 실패로 반복 중단 | `d092-new-repeat-owner.log` |
| D-092 inproc ROUTER 종료 관찰 선택, `--repeat until-fail:10` | 10/10 PASS | `d092-focused-repeat-owner.log` |
| D-092 inproc ROUTER 자동 재연결 선택, `--repeat until-fail:10` | 10/10 PASS; 두 선택 실행의 DISCONNECTED 관찰 9–10ms | `d092-focused-repeat-owner.log` |
| `test_two_pending_connects_then_disconnect` 선택, `--repeat until-fail:10` | 10/10 PASS | `d092-pending-repeat.log` |
| 지정 회귀 regex | 9/9 PASS, 51.99초 | `d092-targeted-owner.log` |
| 최초 전체 ctest | 173/176 PASS. HANDOVER, monitor PAIR 종료 timeout, dev hotpath 실패. Timeout은 위 소유권 수정으로 해소 | `d092-full.log` |
| `test_multi_socket_contract_regressions`, `--repeat until-fail:10` | 10/10 PASS | `d092-multi-owner-repeat.log` |
| 소유권 수정 뒤 최종 전체 ctest, 1회 | **174/176 PASS**, 241.67초. 남은 실패: 신규 HANDOVER case, dev hotpath(n/a). Monitor PAIR 종료 suite는 PASS | `d092-full-final.log` |
| `git diff --check`, 패치 reverse 적용 검사 | PASS | — |

지정 회귀 regex는 `test_inproc_pending_connect_rejected_at_attach|test_router_reject_duplicate|test_socket_disconnect_boundary|test_socket_disconnect_progress_without_app_poll|test_router_reciprocal_handover_lanes|test_monitor`다.
`test_router_reject_duplicate`의 미수신 DATA 보존·request 종결 검사와 reciprocal HANDOVER의 standby
관찰도 통과했다. Hotpath는 dev에도 등록되어 전체 ctest에서 실행되었고 실패했다. 요청대로 dev 성능 판정은 n/a이며
이를 release 성능 회귀로 판정하지 않는다. 기준값은 변경하지 않았다.

## BLOCKERS

**Tcp ROUTER same-RID count-two pair 충돌 — 수정 전부터 존재하는 별도 B 결함.** 신규 테스트의
`test_router_handover_tcp`에서 두 번째 ROUTER가 CONNECTED 뒤 binder의 READY에 도달하지 못한다.
`asio_zmp_engine.cpp:644`가 peer RID로 accepted pair를 찾고,
`socket_base_api.cpp:86-89`가 count-two에 기존 pair ID를 돌려준다.
기준 커밋의 `:330-333`(수정본 `:336-339`)은 이미 점유된 lane에 다른 pipe가 들어온 것으로 판단하여 새 pipe와 이전 pair의
application/completion pipe를 종료한다. `router_admission.cpp`의 HANDOVER 결정 전 실패다.
Baseline의 기존 ROUTER debug trace에도 두 번째 RID adoption 없이 pipe 종료가 나타난다.
Tcp ROUTER REJECT case의 DISCONNECTED 통과 역시 이 선행 충돌이 있으므로 실제 REJECT admission
경로까지 검증한 결과로 해석할 수 없다. Inproc ROUTER에서는 REJECT admission을 직접 재현했다.

공개 API 재현 명령:

```bash
cd /home/hep7/project/zlink-core-b
ZLINK_TEST_CASE=test_router_handover_tcp \
  core/build-dev/bin/test_router_reject_disconnected_without_app_recv
```

이 충돌은 종료 관찰과 다른 pair identity 결정이므로 이번 패치에서 고치지 않았다.
READY 단언, timeout, fixture 조건은 완화하지 않았다. 이 BLOCKER가 해소되기 전에는 신규 전체
반복 gate가 green이 아니며, 패치를 전체 gate 합격으로 판정할 수 없다.
