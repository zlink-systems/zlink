# Raw STREAM 명시적 RID 종료의 DISCONNECTED 수정

Raw TCP/WS STREAM에서 `zlink_disconnect_rid()`로 연결을 닫을 때 누락되던
`DISCONNECTED`를 Core의 공통 physical connection claim으로 게시하도록 수정했다.
Peer close와 명시적 종료가 같은 연결의 이벤트를 중복 게시하지 않으며, pipe를 재사용하는
raw connector도 재연결마다 종료 이벤트를 게시한다. 공개 API와 transport별 종료 절차는 유지한다.

- 작업 트리: `/home/hep7/project/zlink-core-a`, detached
  `bf937cb79ad3fbab00c9d0d87803c6c727576031`.
- 기존 신규 테스트·CMake 등록·조사 보고서와 untracked 산출물을 보존했다.
- Runtime·테스트 수정과 빌드는 worktree a에서만 수행했다. MAIN에는 이 요약 문서만 작성했다.
  `core/doc/spec/`, `bindings/`, `framework/` 소스와 MAIN의 `core/build-dev`는 수정하지 않았다.
- 커밋하지 않았다. 패치는 `/home/hep7/project/zlink-core-a/core-stream-disconnected.patch`다.
- 로그와 검증 artifact hash: worktree a의 `stream-disconnected-logs/`.

## 원인과 수정

원인은 기준 revision의 다음 호출 경로에 있다.

1. `core/src/runtime/sockets/stream/stream.cpp:368`의 `xterm_peer_rid()`가
   `terminate(true)`로 연결 종료를 요청한다.
2. `core/src/runtime/core/session_base.cpp:453`의 peer 종료 callback은 engine의
   `terminate()`를 호출한다. `core/src/runtime/engine/asio/asio_engine.cpp:390`의
   `terminate()`는 `error():1909`의 monitor 게시를 실행하지 않는다.
3. `core/src/runtime/sockets/common/socket_base_api.cpp:1751`은 공통 claim을 획득해도
   `pair_id != 0`일 때만 이벤트를 게시한다. ZMP pair가 없는 raw STREAM은 제외된다.
4. `core/src/runtime/core/session_base.cpp:166`은 raw 연결의 engine error에서 공유 claim을
   건너뛴다. Socket의 pair 조건만 없애면 producer 간 중복 방지가 공통 규칙으로 성립하지 않는다.

이 누락의 선행 증거는
[`core-stream-delivery-regression-599b4a75ef-summary.md`](./core-stream-delivery-regression-599b4a75ef-summary.md)의
공개 C API 재현과 owner 파일 A/B다. 해당 보고서는 `599b4a75ef` 이전
`socket_base_api.cpp` 하나를 별도 컴파일·링크했을 때도 TCP/WS 명시적 종료가 동일하게 실패했다고
기록한다. 이는 그 owner 파일의 전후 대조이며, 과거 revision 전체 rebuild 결과는 아니다.
이번 수정은 세 Framework sample 실패와의 인과관계를 주장하지 않는다.

수정 파일과 역할은 다음과 같다.

| 파일 | 변경 |
|---|---|
| `core/src/runtime/core/session_base.cpp:166` | Raw/paired 구분을 제거하고, socket pipe가 있는 transport는 모두 그 endpoint의 claim을 사용한다. Pipe 생성 전 handshake 실패에 대한 기존 fallback은 유지한다. |
| `core/src/runtime/sockets/common/socket_base_api.cpp:1720` | Claim 획득 뒤의 pair ID 조건을 제거한다. 기존 `pipe_peer_terminated()`에서 raw 연결도 동일하게 게시한다. |
| `core/src/runtime/core/pipe.cpp:3465`, `pipe.hpp:890` | 기존 atomic boolean을 마지막으로 claim한 physical connection ID로 대체한다. 같은 연결의 두 producer 중 하나만 성공하고, pipe 재사용 뒤의 새 연결은 새 claim을 획득한다. 별도 registry·timer·retry 상태는 추가하지 않는다. |
| `core/tests/integration/test_stream_multiclient_delivery.cpp` | 기존 TCP/WS 4개 case를 보존했다. 마지막 잔여 client들의 종료 identity도 대조하고, 기본 `IMMEDIATE=0` connector의 3회 연결·종료 case를 추가했다. |
| `core/tests/CMakeLists.txt:142` | 인계받은 integration/serial 등록을 보존했다. |

대안은 STREAM의 `xterm_peer_rid()` 또는 engine `terminate()`에 별도 게시를 추가하는 것이다.
이 경우 기존 error producer와 중복을 조정할 두 번째 종료 정책이 필요하다. 채택한 수정은
기존 claim과 게시 callback을 사용하고 transport 조건을 제거한다.

단순히 raw 연결에도 기존 boolean claim을 적용한 중간 구현은 listener 4개 case를 통과했지만,
새 connector 재연결 case에서 `Reconnected STREAM transport lost DISCONNECTED`로 실패했다
(`reconnect-before.log`). `session_base.cpp`의 `reconnect()`가 기본 `IMMEDIATE=0`에서 pipe를
유지하므로 claim의 수명도 pipe가 아닌 physical connection에 맞춰야 한다.
최종 구현은 동일한 테스트를 통과한다.

소유 계층: **Core socket/session의 physical connection 종료 관찰**이며, 단일 claim 소유자는
socket endpoint의 `pipe_t::try_claim_transport_disconnected_event()`다.

Spec 조항: `core/doc/spec/core/socket/08-stream.ko.md` §4·§6.3·§9,
`core/doc/spec/core/socket/README.ko.md` §4·§6 `zlink_disconnect_rid`,
`core/doc/spec/core/05-polling.ko.md` §3, `core/doc/spec/core/06-monitoring.ko.md`
§3.1·§9(:539–543)의 transport와 무관한 단일 DISCONNECTED 계약이다.
D-092의 peer 종료 전이에서 게시하는 위치를 유지하고, application recv나 최종 drain까지
관찰을 미루지 않는다. 기존 final release도 같은 callback과 claim을 사용한다.

분류: **B — 기존 Core 결함 수정**. 공개 계약 변경이나 Framework 보상이 아니다.

수정 전/후 규칙 수: **종료 관찰 2 → 1**. Paired의 공통 claim과 raw의 engine 직접 게시를
physical connection당 하나의 공통 claim으로 통합했다. Claim 상태도 하나를 대체했으며 복제하지 않았다.

## Binding·Framework 소비자 대조

Parity: TCP/WS 공개 C API를 실행하고, .NET·Java·Node의 monitor 전달과 STREAM session 정리
소비자를 소스로 대조했다. 세 언어 모두 Core의 DISCONNECTED를 필요로 하므로 Core에서 수정했다.
Binding/Framework 테스트와 sample은 이번 작업에서 실행하지 않았다.

| 계층·언어 | 확인한 소비자와 역할 |
|---|---|
| .NET binding | `bindings/dotnet/src/Zlink/Runtime/Eventing/SocketMonitor.cs:17`의 `Recv()`가 Core monitor recv 결과를 변환한다. |
| Java binding | `bindings/java/src/main/java/systems/zlink/runtime/eventing/NativeMonitorSocket.java:50`의 `recv()`가 `Native.monitorRecv()` 결과를 전달한다. |
| Node binding | `bindings/node/src/zlink/runtime/eventing/monitor_socket.ts:24`의 `MonitorSocket.recv()`가 native monitor 결과를 변환한다. |
| .NET Framework | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Streams/ZLinkStreamNodeRuntime.cs:643`이 DISCONNECTED의 RID로 session을 찾아 `EnqueueDisconnected()`를 호출한다. `ZLinkStreamSessionRuntime.cs:689`의 `CompleteSessionAsync()`는 종료 callback 뒤 dispose를 예약하고, `:196`의 dispose는 context 정리와 session 제거를 수행한다. |
| Java Framework | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocket.java:176`의 `onTransportError()`가 DISCONNECTED monitor를 소비한다. `runtime/streams/ZLinkStreamRuntime.java:1113`의 `reportTransportError()`가 peer·session을 제거하고 disconnect lifecycle을 실행한다. |
| Node Framework | `framework/languages/node/packages/framework/src/runtime/streams/stream-session-runtime.ts:1144`가 DISCONNECTED를 session의 `enqueueDisconnected()`로 전달한다. `:752`의 `queueDisconnect()`와 `:767`의 `cleanup()`이 transport 종료, binding 정리와 session 제거를 수행한다. |

## 검증 결과

모든 실행은 worktree a의 `core/build-dev`를 사용했다.

| 검증 | 결과 | 로그 |
|---|---|---|
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS, RelWithDebInfo / LTO OFF | `build-final.log` |
| `test_stream_multiclient_delivery --repeat until-fail:10` | PASS, 5 case × 10회, 8.16초 | `repeat.log` |
| 지정 STREAM·D-092·monitor·transport matrix·disconnect boundary regex, `-j2` | **23/23 PASS**, 25.78초 | `targeted.log` |
| 전체 ctest `-j2 -E '^hotpath_gate$'`, 1회 | **176/176 PASS**, 235.08초 | `full.log` |
| `git diff --check`, 보호 경로 무변경, 패치 reverse 적용 검사 | PASS | 변경 diff와 `git apply --reverse --check` |

Hotpath gate는 요청의 `hotpath n/a`에 따라 제외했다. 성능 합격으로 세지 않는다.
재현 명령은 다음과 같다.

```bash
ctest --test-dir core/build-dev -R '^test_stream_multiclient_delivery$' --repeat until-fail:10 --output-on-failure
ctest --test-dir core/build-dev -R 'test_stream|test_router_reject_disconnected_without_app_recv|test_monitor|test_transport_matrix|test_socket_disconnect_boundary' --output-on-failure -j2
ctest --test-dir core/build-dev --output-on-failure -j2 -E '^hotpath_gate$'
```

패치는 `core/`에서 `git add -N . && git diff HEAD > /home/hep7/project/zlink-core-a/core-stream-disconnected.patch`로
생성했다. 인계받은 신규 테스트·CMake 등록과 이미 index에 있던 기존 조사 보고서도 포함한다.
기존 untracked 패치·로그는 보존하고 포함하지 않았다. MAIN의 이 요약 문서는 별도 산출물이다.

## BLOCKERS

- 이 Core 종료 이벤트 수정의 미해결 blocker와 최종 테스트 실패는 없다.
- Hotpath와 Binding/Framework 실행 검증은 이번 범위에서 제외했다. 기존 보고서의 세 sample
  회귀 원인까지 해소했다는 판정에는 사용할 수 없다.
