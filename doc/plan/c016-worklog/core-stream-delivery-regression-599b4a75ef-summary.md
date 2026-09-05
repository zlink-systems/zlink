# STREAM 전달 회귀 조사 — 599b4a75ef

요청한 세 sample 실패의 공통 원인을 Core에서 확인하지 못했다. **Core runtime 수정은 없다.**
새 공개 C API 검사에서 TCP/WS의 다중 client 전달 순서는 각각 10회 통과했다.
`zlink_disconnect_rid()`의 STREAM DISCONNECTED 누락은 재현했으나, 종료 소유 파일을
`599b4a75ef` 이전 버전으로 교체한 A/B에서도 같은 실패가 발생했다.
따라서 이 결함을 수정하거나 snapshot의 command drain을 복구하는 것을 sample 회귀 수정으로
제출하지 않는다. 이 패치는 진단과 미충족 계약을 보존하며, 합격한 runtime 수정 패치가 아니다.

- 작업 트리: `/home/hep7/project/zlink-core-a`, detached `1c0c39c5aeb90361c365489a4c97cf0165d52f9c`.
- 마지막 Core 소스 변경: `599b4a75efa8b60eae76db1e462120193594772a`.
- 요청한 reset/clean/detached checkout을 수행했다. 이후 branch 변경과 commit은 하지 않았다.
- `core/src`, spec, bindings, framework와 main의 `core/build-dev`는 수정하지 않았다.
- 기존 untracked `core-explicit-not-found.patch`, `explicit-removal-logs/`는 보존하고 패치에서 제외한다.
- 빌드·재현 로그와 A/B 실행파일: 작업 트리의 `stream-regression-logs/`.
- 패치: `/home/hep7/project/zlink-core-a/core-stream-regression.patch`.

## 변경 hunk와 STREAM 호출 경로

`git show 599b4a75ef -- core/src`의 모든 변경을 다음 경계로 나누어 확인했다.
아래의 미재현 판정은 이 검사에 대한 결과이며, upstream ROUTER 경로 전체가 정상이라는 뜻은 아니다.

| 변경 | 확인한 소유 경계와 결과 |
|---|---|
| `socket_base_api.cpp:1720` peer 종료에서 DISCONNECTED 게시·inproc reconnect 예약 | `pipe.cpp:2935-2968`은 실제 peer term 상태 전이 뒤 callback을 호출한다. STREAM route attach/supersession이 이 callback을 호출하는 경로는 없다. Inproc reconnect는 connector의 inproc 등록을 조건으로 하므로 TCP/WS STREAM listener의 reconnect를 예약하지 않는다. |
| 같은 파일 `:66-103` accepted pair 재사용 조건, `socket_base.hpp` 주석 | 호출자는 `asio_zmp_engine.cpp:644`다. Raw STREAM은 `asio_raw_engine_t`를 사용하며 이 READY 협상에 진입하지 않는다. Count-one 할당 동작도 이 hunk 전후 동일하다. |
| 같은 파일 `:261-279` endpoint 소유권과 종료 ack 등록 | 아직 final ack가 끝나지 않은 pipe를 소켓이 소유한다. Scheduler 등록에는 별도로 `:464-487`의 기존 `is_lifecycle_active()` 검사가 남아 있다. Inactive pipe의 소유와 활성 route 등록을 혼동하지 않는다. |
| 같은 파일의 final `pipe_terminated`에서 event·reconnect 제거 | 기존 event claim을 앞선 peer callback으로 옮겼다. STREAM의 물리적 연결 identity는 ZMP pair와 별개다. 아래의 raw 종료 누락은 전후 모두 존재한다. |
| `socket_base_monitor.cpp:219-230` snapshot의 `process_commands()` 제거 | 호출자는 generic RID 제거, endpoint 제거, local peer-weight 전파, DEALER submit 대상 구체화다. STREAM은 `stream.cpp:368`에서 RID 제거를 override하고 `:996-1073`에서 route에 직접 쓰고 flush한다. DEALER submit도 `socket_send_submit.cpp:300`에서 command를 진행한 뒤 snapshot한다. Snapshot이 STREAM push flush를 담당하는 호출 경로는 찾지 못했다. |
| `socket_base_endpoint.cpp`, `socket_endpoint_runtime.cpp` pending-inproc materialization 이동 | Inproc connect intent 제거 후 pending peer를 처리한다. Raw network STREAM 전달 경로가 아니다. |
| `router_admission.cpp:454` superseded pair의 request 종결 | ROUTER의 논리적 교체가 물리적 종료 callback을 호출하지 않도록 기존 request owner를 직접 호출한다. STREAM 자체에는 이 RID 중복 정책이 적용되지 않는다. |

Raw STREAM의 연결 식별은 `stream.cpp:52-75`에서 확인할 수 있다. ZMP pair가 없으면
연결마다 바뀌는 transport connection identity를 사용한다. 따라서 “Application lane 하나”라는
표현과 “ZMP count-one pair registry에 등록된다”는 표현을 같은 의미로 사용할 수 없다.
TCP listener는 `asio_tcp_listener.cpp:245`에서 raw engine을 생성한다.

## 공개 C API 재현과 별도 종료 결함

추가 파일은 `core/tests/integration/test_stream_multiclient_delivery.cpp`이며
`core/tests/CMakeLists.txt`에 integration/serial로 등록했다. Core 내부 심볼이나 failpoint를
호출하지 않는다. 외부 client는 Boost.Asio TCP/Beast WebSocket을 사용한다.

검사 순서는 다음과 같다.

1. PACKET mode STREAM server에 client 4개를 연결한다. Monitor READY만으로 각 RID를 얻고,
   연결 identity와 RID의 중복이 없는지 확인한다.
2. Server application recv/poll 없이 각 client에 작은 packet과 128 KiB packet을 교차 전송한다.
   DONTWAIT submit은 모두 OK, completion ID는 0이어야 한다. Client별 전체 byte 열을 제출한
   packet의 연결 결과와 비교한다. WebSocket frame 경계와 application packet 경계를 혼동하지 않는다.
3. Client가 자기 번호를 넣은 packet을 보내고, server가 공개 packet recv로 source RID와 번호를 대조한다.
4. Client 하나를 닫고 그 연결의 DISCONNECTED를 확인한다. 남은 client의 push, 새 client의
   새 identity와 이후 전달을 검사한다. Peer close와 server `zlink_disconnect_rid()`를 TCP/WS에서 각각 실행한다.

Peer close 뒤 전달은 TCP/WS 모두 통과했다. **명시적 RID 제거에서는 DATA 순서가 깨진 것이 아니라
DISCONNECTED가 오지 않았다.** 최초 실패에서 client와 monitor를 정리한 뒤 실패를 보고하도록 하여
관찰 실패 때문에 test context 종료까지 막히지 않도록 했다. Assertion과 대기 시간은 완화하지 않았다.

명시적 종료의 sequence와 원인은 다음과 같다.

1. `stream.cpp:368-397`의 `xterm_peer_rid()`가 해당 pipe를 `terminate(true)`로 닫는다.
2. Session의 `session_base.cpp:453-462`가 종료를 받아 engine의 `terminate()`를 호출한다.
3. `asio_engine.cpp:390-406`의 `terminate()`는 `error()`의 DISCONNECTED 게시 경로를 실행하지 않는다.
4. Socket callback `socket_base_api.cpp:1751`은 `pair_id != 0`일 때만 DISCONNECTED를 게시한다.
   ZMP pair가 없는 raw STREAM은 이 조건을 통과하지 못한다.

변경 전 `socket_base_api.cpp:1762`에도 동일한 `pair_id != 0` 조건이 있었다. 이를 실험으로
대조하기 위해 parent revision의 **이 소유 파일 하나만** 별도 object로 컴파일하고, 같은 공개
테스트 object와 현재 static archive보다 먼저 링크했다. Library·작업 소스·build tree의 Core를
교체하지 않았고 여러 revision의 Core를 반복 빌드하지 않았다. 이 A/B는 완전한 rebuild7 재현이
아니며, 해당 소유 파일의 변경 전후 비교다. TCP/WS 모두 같은 종료 event 누락으로 실패했다.

소유 계층: 확인된 종료 누락은 Core socket/session의 물리적 종료 event 게시 경계다.

Spec 조항: `core/doc/spec/core/socket/08-stream.ko.md` §4·§6.3·§9,
`socket/README.ko.md` §4·§6, `05-polling.ko.md` §3,
`06-monitoring.ko.md` §3.1 및 §9(:539-543)의 transport와 무관한 단일 DISCONNECTED 계약이다.

Parity: 공개 C API에서 TCP/WS를 대조했다. 세 언어 sample 기록도 대조했으나, sample 재실행이나
언어별 runtime 수정은 하지 않았다. C API 재현은 언어별 보상 없이 같은 Core 소유 경계를 검사한다.

분류: 별도 DISCONNECTED 누락은 **B — 기존 Core 결함**이다. 요청한 세 sample 회귀는 원인과
A/B/C/D 분류를 아직 확정할 수 없다. Spec gap으로 판정한 것은 아니다.

수정 전/후 규칙 수: **종료 관찰 2 → 2**, runtime 변경 0. Paired transport는 socket/engine의
공유 claim을, raw transport는 engine 게시를 사용한다. 별도 종료 결함을 고친다면 engine과
socket이 사용하는 physical event claim을 통일하는 대안이 STREAM 전용 event 분기보다 적은 규칙을
요구한다. 이번에는 sample 회귀와의 인과관계가 없어 이 설계 변경을 구현하지 않았다.

## Sample 증거의 해석

### .NET Bingo

지정한 `logs/` 외에 같은 run의 `sample-logs/`에 message-flow와 예외가 보존돼 있었다.
`sample-logs/play-a.log:296-300`은 PlayerJoined/BingoGameStarted의 stream send 성공을 기록하며
client에서도 수신을 확인할 수 있다. 해당 run에서 BingoNumberDrawn의 성공한 stream send
기록은 찾지 못했다.

`:526-534`에는 draw timer가 `No current session binding exists for actor 'player-1'`로 실패한
stack이 있다. Throw 지점은 `ZLinkBoundSessionService.cs:172`, 호출자는
`BingoNotificationPublisher.cs:44`다. 다만 그 앞 `:513-525`에 actor disconnect가 있으므로 이 예외는
timeout 이후 cleanup의 결과일 수 있다. 이 stack을 최초 notify 누락의 root cause로 단정하지 않는다.

### Node DeliveryDispatch

`work/deliverydispatch-evidence.jsonl:1-3`의 업무 기록은
`delivery-success: Assigned → Reassigned → Failed`다. `logs/dispatch.log:6`도
`reason=candidates-exhausted`를 기록한다. 반면
`Client/deliverydispatch-client-scenario.ts:69-74`는 이 delivery에
`Assigned → Accepted → PickedUp → Delivered`를 기대한다.
따라서 browser의 “expected sequence” 예외만으로 wire 순서 역전을 증명할 수 없다.
Offer/decision이 왜 완료되지 않았는지는 별도 추적이 필요하다. 설정된 `logs/flow/`에서 이 run의
flow 파일은 찾지 못했다.

### Java DeliveryDispatch

지정 gate log와 이 revision의 Java 조사 brief는 업무 시나리오 완료 뒤 courier-session만
TEARDOWN_FAILED로 끝난 사실을 보여 준다. 제공 구간에는 실패 exception/stage가 없다.
이것만으로 정상 전달 중 packet 유실·역전과 같은 원인이라고 판단할 수 없다.

## 검증 결과

| 검증 | 결과 | 로그 |
|---|---|---|
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS, RelWithDebInfo / no LTO | `build-baseline.log`, `build-final.log` |
| 신규 전체 `--repeat until-fail:10` | 첫 회 중단: 4 case 중 peer-close 2 PASS, explicit-disconnect 2 FAIL | `new-repeat-final.log` |
| TCP peer-close 선택 `--repeat until-fail:10` | 10/10 PASS | `new-tcp-repeat.log` |
| WS peer-close 선택 `--repeat until-fail:10` | 10/10 PASS | `new-ws-repeat.log` |
| 변경 전 종료 owner와 링크한 A/B | 동일한 2 PASS / 2 FAIL | `new-pre599-owner.log`, `build-owner-ab.log`, `link-owner-ab.log` |
| 지정 regex `-j2` | 19/20 PASS, 30.77초. 신규 종료 관찰 검사만 FAIL | `targeted.log` |
| 전체 ctest `-j2`, 1회 | **175/177 PASS**, 246.71초. 신규 종료 관찰 검사와 dev `hotpath_gate` FAIL | `full.log` |
| `git diff --check`, Core runtime 무변경 확인 | PASS | `git diff --exit-code -- core/src` |

전체 ctest 뒤 신규 검사에 “peer 종료 뒤 새 accept 전에 남은 client에게 push” 단언을
추가했다. 최종 신규 검사만 다시 빌드·반복했으며 전체 ctest는 반복하지 않았다.
Core runtime은 모든 실행에서 동일하다. Build artifact hash는
`stream-regression-logs/artifacts.json`에 보존했다.

재현 명령은 다음과 같다. 첫 명령은 확인된 종료 누락으로 실패한다. 전달 검사만 선택하려면
`ZLINK_TEST_CASE=test_stream_tcp_peer_close_delivery` 또는
`ZLINK_TEST_CASE=test_stream_ws_peer_close_delivery`를 설정하고 같은 ctest 명령을 실행한다.

```bash
ctest --test-dir core/build-dev -R '^test_stream_multiclient_delivery$' --repeat until-fail:10 --output-on-failure
stream-regression-logs/test_stream_multiclient_delivery.pre599-owner
ctest --test-dir core/build-dev -R 'test_stream|test_router_reject_disconnected_without_app_recv|test_router_reject_duplicate|test_socket_disconnect_boundary|test_transport_matrix' --output-on-failure -j2
```

Dev hotpath 측정은 reference의 1.258–1.317배로 실패했다. Runtime 변경 없이 발생한 결과이며,
기존 두 변경 보고서도 dev hotpath 실패를 기록한다. Release 성능 회귀 여부는 이번에 검증하지
않았고, 이 실패를 합격으로 세거나 reference를 수정하지 않았다.

## BLOCKERS

- 요청한 `599b4a75ef` → 세 sample 회귀의 인과관계와 실패하는 Core 전달 sequence를 확인하지 못했다.
  Send admission부터 실제 client 수신까지 같은 flow를 연결한 실패 기록 또는 공개 C API repro가 필요하다.
- 새 테스트에서 확인한 STREAM explicit-disconnect event 누락은 변경 전 owner에서도 실패한다.
  이를 이유로 관련 없는 runtime을 수정하거나 assertion을 낮추지 않았다. 신규 전체 10회 gate는 미충족이다.
- Java teardown의 실제 exception과 소유 stage, Node offer/decision의 마지막 성공 transition,
  .NET 첫 draw의 submit 전후 transition은 현재 증거만으로 확정할 수 없다.
- 전체 ctest는 신규 종료 관찰 검사와 dev hotpath가 남아 있으므로 green이 아니다.
  Sample 회귀 수정 완료 또는 배포 검증 완료로 사용할 수 없다.
