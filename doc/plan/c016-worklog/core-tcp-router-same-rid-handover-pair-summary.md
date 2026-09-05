# TCP ROUTER same-RID HANDOVER의 accepted pair 충돌 수정

READY된 TCP ROUTER pair와 같은 RID로 연결한 두 번째 ROUTER가 새 pair를 받아
ROUTER admission에 도달한다. HANDOVER는 새 route를 채택하고 이전 physical lane을
standby로 유지한다. REJECT는 새 pair만 닫으며 기존 route는 계속 request/reply를 처리한다.
D-092 작업의 TCP HANDOVER BLOCKER를 해소했다.

- 작업 트리: `/home/hep7/project/zlink-core-b`, detached `b63f79a3ce`.
- 기존 D-092의 6개 파일 변경을 보존하고 그 위에 수정했다. 커밋하지 않았다.
- 통합 패치: `/home/hep7/project/zlink-core-b/core-d092-plus-handover.patch`.
- Main에는 이 보고서만 작성했다. Main의 `core/build-dev`, spec, bindings, framework는 수정하지 않았다.

## 원인과 수정

기존 `core/src/runtime/engine/asio/asio_zmp_engine.cpp:644`는 accepted READY의 peer RID로
`adopt_accepted_transport_pair()`를 호출한다. D-086의 `7ffb8e55d9`는 count-one 연결에만
새 pair ID를 주었고, 기존 `core/src/runtime/sockets/common/socket_base_api.cpp:86-89`는
count-two이면 READY 여부와 관계없이 기존 RID의 pair ID를 반환했다. 따라서 새 Application
pipe가 기존 pair의 점유된 lane에 들어갔다. D-092 작업본의 `socket_base_api.cpp:336-339`는
이를 중복 lane으로 판단하여 양쪽 pair pipe를 종료했다. ROUTER의 RID 정책을 실행하기 전의 실패다.

수정본 `socket_base_api.cpp:86-103`은 기존 pair table의 `ready`를 같은 mutex 안에서 읽는다.
미완성 count-two lane set에만 sibling을 결합하고, READY된 pair와 같은 RID의 새 handshake에는
기존 allocator로 새 ID를 준다. 이전 pair는 자기 ID로 table에 남으며, ID를 확인하는 기존 release가
새 handshake의 registry entry를 지우지 못한다. RID를 기준으로 READY된 pair를 무조건 재사용하던
예외를 제거했다. 새 상태, timer, retry, public API 또는 wire property는 추가하지 않았다.

Pair ID의 할당·lane 결합 소유자는 Core socket의 transport-pair registry다. Engine은 handshake
정보를 전달하고 할당받은 identity를 session에 설정한다. Engine마다 무조건 새 pair ID를 주는
대안은 count-two의 두 physical connection을 서로 다른 pair로 나누므로 채택하지 않았다.
RID 중복의 HANDOVER/REJECT 결정은 `router_admission.cpp` 한 곳에 남는다.

D-B112의 `connection_id`는 physical attempt마다 생성하는 로컬 monitor 진단값이다.
두 lane의 공통 key나 원격 pair token이 아니다. ZMP §4.1도 connection ID/generation을
wire property로 보내지 않는다고 규정한다. 따라서 endpoint 주소나 monitor ID를 사용한
별도 pairing 규칙을 만들지 않았다. 동시에 미완성인 같은 RID lane set을 구분하는 새 protocol도
도입하지 않았다. 기존 §4.1의 미완성·중복 lane 검증은 유지한다.

## REJECT admission 뒤 드러난 snapshot 재진입 결함

강화한 공개 API 테스트의 반복 실행에서 endpoint iterator segfault도 검출했다.
GDB는 `socket_base_endpoint.cpp:1094`의 map iterator 증가에서 멈췄다.
`term_endpoint_internal()`은 `:1085`에서 range를 얻고 `:1092`에서 pending request를 정리한다.
그 경로의 `:1020`은 `snapshot_attached_pipes()`를 호출하는데, 기존
`socket_base_monitor.cpp:224`가 숨겨진 `process_commands()`를 실행했다. Count-two 종료로
대기 중이던 다른 `term_endpoint` command가 같은 range를 삭제할 수 있었다.

`socket_base_monitor.cpp:224-225`에서 그 drain을 제거하여 snapshot은 현재 pipe 목록만 복사한다.
Disconnect와 routed-submit 호출 경로는 이미 자기 진입점에서 command를 진행한다.
Peer-weight 변경은 현재 pipe에 전파하고 나중에 attach되는 pipe에는 기존 초기화 경로가 현재 값을
적용하므로 snapshot 내부 drain이 필요하지 않다. 새 lock, iterator 복구 또는 재시도를 추가하지 않았다.
이 수정 뒤 신규 전체 테스트가 10회 연속 통과했다.

- 소유 계층: Core transport-pair registry가 physical lane set identity를, ROUTER admission이 RID 정책을, command 진입점이 진행을 소유한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md` §4 RID 중복 정책·§6 request completion/disconnect, `core/doc/spec/core/protocol/01-zmp.ko.md` §4.1, `core/doc/spec/core/06-monitoring.ko.md` §3.1. D-086/D-B94·D-B96·D-088·D-090·D-092·D-B112 적용.
- Parity: 공개 C API로 TCP/inproc × ROUTER/DEALER × HANDOVER/REJECT를 대조했다. C++ `bindings/cpp/src/Runtime/Eventing/monitor.cpp:148`와 .NET `bindings/dotnet/src/Zlink/Runtime/Eventing/SocketMonitor.cs:20`은 같은 Core monitor API를 호출한다. 언어별 보상이나 Framework runtime 변경은 없다.
- 분류: **B — 기존 Core 결함**. READY pair 재사용과 snapshot의 숨은 command 재진입을 소유 모듈에서 수정했다.
- 수정 전/후 규칙 수: **READY pair의 RID 중복 결정 지점 2 → 1**(pair-table 선행 거부 + ROUTER 정책 → ROUTER 정책), **snapshot 책임 2 → 1**(command drain + 목록 복사 → 목록 복사). Lane topology 검증은 기존 pair table 하나가 계속 소유한다.

## 회귀 검증

`core/tests/integration/test_router_reject_disconnected_without_app_recv.cpp:175`의 기존
9개 case를 유지하고, `:295`에 TCP reconnect case를 추가했다.

- HANDOVER: 서로 다른 binder READY connection ID, 이전 pair request의 `NOT_CONNECTED`,
  중복 completion 부재, 새 connector의 request/reply 성공을 검사한다. 이전 connector는 app recv를
  호출하지 않으며 DISCONNECTED가 없다. Binder를 실제로 닫은 뒤에만 그 연결의 DISCONNECTED를 확인한다.
- REJECT: 거부된 connector가 app recv/poll 없이 200ms 안에 DISCONNECTED를 관찰한다.
  Binder/기존 connector에 protocol failure나 기존 연결의 DISCONNECTED가 없어야 하며,
  기존 ROUTER connector와의 request/reply도 성공해야 한다. Pair 충돌로 양쪽을 닫는 구현은 통과할 수 없다.
- Reconnect: 같은 binder에 TCP endpoint 두 개를 만든다. 첫 listener의 unbind로 client transport를
  끊되 client connect intent는 유지한다. 두 번째 endpoint에 같은 RID의 다른 ROUTER를 admission한 뒤
  첫 listener를 다시 bind한다. Client의 자동 재연결이 새 connection identity로 READY에 도달하고,
  다른 endpoint의 기존 route를 인수하며, 그 route의 pending request는 `NOT_CONNECTED`로 종결된다.
  새 route의 request/reply와 기존 physical lane의 standby 유지도 확인한다.

| 검증 | 결과 | Worktree 로그 |
|---|---|---|
| 수정 전 `ZLINK_TEST_CASE=test_router_handover_tcp` | binder READY 3000ms 초과 재현 | `handover-baseline.log` |
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS, RelWithDebInfo / LTO OFF | `handover-build-final.log` |
| 신규 전체 `--repeat until-fail:10` | **10/10 PASS, 100 case 실행**, 3.75초 | `handover-repeat-final.log` |
| 지정 회귀 8개 + `test_zmp_metadata` | **9/9 PASS**, 64.12초 | `handover-targeted.log` |
| 전체 ctest, `-j2`, 1회 | **175/176 PASS**, 239.42초. 유일 실패: dev hotpath(n/a) | `handover-full.log` |
| `git diff --check`, 통합 패치 reverse 적용 검사 | PASS | — |
| 기존 D-092 runtime/CMake diff reverse 적용 검사 | PASS, 기존 수정 보존 확인 | — |

지정 회귀는 `test_router_reject_duplicate`, `test_socket_disconnect_boundary`,
`test_router_reciprocal_handover_lanes`, `test_ctx_term_fixed_rid_handover`,
`test_router_handover`, `test_transport_matrix`, `test_monitor_connection_identity`,
`test_multi_socket_contract_regressions`다. Snapshot 결함의 backtrace는
`handover-repeat-gdb.log`에 보존했다. 임시 runtime logging은 추가하지 않았다.

통합 diff는 8개 Core 파일이다. 이 작업의 추가 변경은 `socket_base_api.cpp`의 pair 할당,
`socket_base.hpp`의 소유권 주석, `socket_base_monitor.cpp`의 snapshot과 기존 신규 테스트다.
나머지 D-092 runtime/CMake 변경은 그대로 포함한다. `core/` 안에서 `git add -N .`을 실행하고
`git diff HEAD`를 패치로 저장하여 기존 untracked 패치와 `diag-node-old-core/`를 제외했다.

## BLOCKERS

기능 gate BLOCKER는 없다. 기존 TCP HANDOVER READY 실패와 신규 반복 중 검출한 endpoint iterator segfault를 해소했다.
전체 ctest의 유일한 실패는 `hotpath_gate`다. Dev 빌드의 성능 판정은 요청대로 n/a이며
이를 성능 합격으로 세지 않는다. Release 성능 측정이나 기준값 변경은 하지 않았다.
