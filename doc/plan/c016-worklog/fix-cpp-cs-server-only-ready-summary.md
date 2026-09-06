# C++ ClientServer Server-only readiness 수정 결과

Server-only host가 Serving이고 local Server의 weight가 100이면
`client_server_runtime_t::is_ready()`는 true, snapshot의 `ready_server_count`는 1을 반환한다.
`selectable`은 false이며 send/request는 기존 `NotConfigured` 경계를 유지한다.
Public API 변경과 commit은 없다.

Debug preset build와 ClientServer focused test는 통과했다. 최초 전체 unit gate는 **50/51**,
sample gate는 **6/7**이다. Fanout unit 실패는 분리 재검증에서 통과했지만, 전체 unit의
0 failures와 sample 7/7 조건은 미충족이다. 아래에 원본 실패와 보존 로그를 기록한다.

소유 계층: Framework ClientServer monitoring이 topology readiness와 Ready Server 집계를 소유한다.
Host 상태는 기존 `framework_runtime_t::status()`에서 읽는다. Outbound 역할 검사는
`framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:536`,
실제 연결 선택은 `framework/languages/cpp/framework/src/runtime/client_server/client_server_location_runtime.cpp:1549`의 기존 경로가 소유한다.

소유 spec: `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:174,179,194`의
topology 범위·host Serving 조건과
`framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:379`의
local·remote positive-weight Ready Server 집계다. C++ 공개 projection은
`framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md:269,307`의
snapshot/is_ready이며, outbound 역할 계약은 공통 `02-channel-transport/03-client-server-channel.ko.md:53`을 따른다.

교차언어 대조 결과: .NET `ZLinkClientServerRuntimeService.cs:98,138`은 host Serving과 Ready Server 수로
readiness를 판단하고 동일 count를 공개 status에 투영한다. C++에서는 `selectable`이 공개 snapshot 필드이므로
.NET의 내부 필드 교체를 복사하지 않고 실제 Client 연결의 선택 가능 여부로 유지했다.
대조 근거는 .NET 참조 수정과 `ClientServerChannelRuntimeTests.cs:632`이며, 이 작업에서 .NET test를 실행하지 않았다.

변경 분류: **B — 기존 결함**. 감독의 지정 수정 작업을 B 구현 승인으로 적용하고, 구현 전에
원인·소유 spec·교차언어 대조를 보고했다.

## 원인과 수정

수정 전 C++ 소스는 사전 parity 표와 달리 Server-only에서 count **0** / false를 반환한다.
`client_server_location_runtime.cpp`의 기존 `append_server`는 `count_as_client_target`이 true일 때만
집계했고 local Server 호출에는 false를 넘겼다(수정 전 :401–407,447–470).
또한 :471의 `Client enabled && count > 0`을 :530의 `is_ready()`가 반환했다.
기존 코드에 새 공개 API 회귀 테스트만 추가한 실행에서 local count assertion 실패를 확인했다
(`focused-red.log`, exit 8).

수정된 구현 경로는 `framework/languages/cpp/` 기준이다.

- `framework/src/runtime/client_server/client_server_location_runtime.cpp:423`:
  `selectable`은 실제 Client 연결 중 Ready 연결이 존재하는지를 나타낸다.
- 같은 파일 :438–463: local Server 상태는 local descriptor에서 읽는다. 같은 RID/generation의
  self 연결이 있으면 그 entry를 local snapshot으로 합쳐 한 번만 집계한다.
  이로써 아직 준비되지 않은 self 연결이 이미 Ready인 local Server를 가리지 않는다.
  별도의 문자열 identity set은 제거했다.
- 같은 파일 :464: 합쳐진 Server 목록의 `ready`를 한 번 집계한다. 양수 weight 조건은 각 Server의
  기존 readiness 판정에서 소유하므로 집계에서 다시 검사하지 않는다.
- 같은 파일 :524: `is_ready()`는 기존 host readiness와 집계 결과를 사용한다.
  `service_provider_t` handle을 복사해 기존 host service를 조회하며 별도 lifecycle 상태를 저장하지 않는다.

대안으로 `selectable`에 Server 역할 예외를 더하는 방식을 검토했다. 이 방식은 host 상태·weight·
local count를 따로 보정해야 하므로 채택하지 않았다. 선택한 구현은 local/remote Server 상태를 합쳐
집계하고, topology readiness와 outbound 선택 가능 여부를 각 소유 경로에서 판정한다.

수정 전/후 규칙 수: **집계·selectable·is_ready의 의미 조건 6 → 4 (3+2+1 → 1+1+2)**.
집계의 Client-target 조건과 중복 weight 검사를 제거했고, 별도 identity set도 1 → 0으로 줄였다.
새 timer, retry, poller, runtime helper 또는 public API는 추가하지 않았다.

## 공개 API 회귀 테스트

`tests/Zlink.Framework.UnitTests/test_cpp_framework_client_server_runtime.cpp:61,81`에서 실제
`app_t`, 공개 builder, hosted service, runtime snapshot/status와 channel client를 사용한다.
추가한 테스트에는 내부 구조 assertion이 없다. Server는 공개 `server().listen()`으로 구성하며,
Client-only는 Ready Server가 없는 manual TCP endpoint, Client+Server는 기존 self-discovery 경로를 사용한다.

| Serving host 구성 | is_ready | ready_server_count | selectable |
|---|---:|---:|---:|
| Server-only, weight 100 | true | 1 | false |
| Server-only, weight 0 | false | 0 | false |
| Client-only, Ready Server 없음 | false | 0 | false |
| Client+Server, weight 100, self 연결 Ready | true | 1 | true |

시작 전과 종료 후에는 false/count 0을 검증한다. 마지막 hosted service의 시작 시점에는 host가
Preparing이고 local positive-weight Server count가 1이어도 `is_ready()`가 false임을 검증한다.
Client+Server는 시작 중과 self 연결 Ready 이후 local Server가 한 번만 집계되는지 검증한다. Ready Server-only의 공개
send/request가 `NotConfigured`로 끝나는 것도 확인한다. 기존 원격 Client readiness/observation
테스트(:200)도 동일 focused target에서 통과했다.

## Gate 결과와 로그

증거 root: `/tmp/zlink-cpp-cs-server-only-ready-20260906/`.
언어 잠금은 `/tmp/zlink-cpp-gate.lock`을 사용했고 sample은 `/tmp/zlink-samples-gate.lock`도 함께
획득했다. 전체 unit 시작 전 loadavg는 **1.47 / 6.70 / 9.90**, sample 시작 전에는
**8.06 / 8.48 / 9.60**이었다. 각 gate는 세 값 모두 10 미만인 것을 확인한 뒤 시작했다.
빌드와 sample은 `build/linux-ninja-debug` 및 기존 C++/Core package 0.17.0을 사용했다.

| 검증 | 결과 | 증거 파일 |
|---|---|---|
| `cmake --build --preset linux-ninja-debug --parallel 2` | PASS, exit 0 | `build-gate.log` |
| ClientServer focused CTest | PASS 1/1 | `focused.log` |
| `ctest --test-dir build/linux-ninja-debug -L framework-unit --parallel 1 --output-on-failure` | FAIL 50/51, exit 8 | `unit-gate.log`, `unit-LastTest.log` |
| 실패한 Fanout GTest 단독 분리 실행 | PASS 1/1 | `fanout-isolated.log` |
| `test_cpp_framework_store_location_resolvers` CTest target 전체 재검증 | PASS 1/1, 내부 40 cases | `location-resolvers-focused.log` |
| `bash samples/run_samples.sh` | FAIL 6/7, exit 1 | `samples.log` |

TicTacToe, Bingo, DeliveryDispatch, SupportChat, GameQuest와 ShoppingMall은 통과했다.
GameQuest의 Killed 출력은 기존 runner :343의 owner kill 시나리오다.
Sample file/flow log는 기존 기능을 사용했으며 cleanup 전에 `sample-evidence/`로 복사했다.
실행 스크립트와 부하 기록도 증거 root에 보존했다. 전체 gate는 반복하지 않았다.

## 남은 실패

**Fanout scope 종료 assertion.** 최초 unit gate의
`ZLinkFrameworkStoreLocationResolvers.AppFanoutPublishUsesLocationAutoConnect`는
`test_cpp_framework_store_location_resolvers.cpp:2983`에서 scoped dependency 생성 4 / 해제 3으로
실패했다. 해당 fixture는 Fanout-only이며 ClientServer를 등록하지 않는다(:2937–2970).
단독 실행과 소속 CTest target 전체 재실행에서는 통과했다. Fanout 구현과 assertion은 수정하지 않았다.

**ZoneWorld G4 crash boundary.** B8은 통과했으나 G4에서
`zoneworld-g4=failed reason=boundary-or-fresh-actor-proof`가 발생했다(`samples.log:59`).
보존 run은 `sample-tmp/tmp.WeQwRfNMYV/`다.

- `logs/client-g4.log:1–2`: armed 이후 stream connector wait timeout으로 종료했다.
  `samples/ZoneWorld/Client/main.cpp:658–664`의 `Unavailable` 응답 대기가 실패한 지점이다.
- `logs/zone-node-1.log:31`: owner 종료 전에 `zoneworld-crash-boundary join pending`에 도달했다.
- `logs/zone-node-2.log:194–195`: 기존 message-flow에 `JoinSpot`, `phase=replied`,
  `outcome=failed`, `reason=activation_rejected`와 `zoneworld-join-failed ... kind=7`이 기록됐다.
- `logs/client-g4-fresh.log`: replacement 이후 fresh actor proof 24개는 출력됐다.
  기존 crash boundary의 `Unavailable` 응답 대기가 실패했으며, 응답 부재인지 다른 오류 응답인지는
  보존 로그만으로 확정하지 않았다.

ZoneWorld는 Gateway :266, ZoneNode :938, Ops :488에서 RouteMesh를 등록하며 ClientServer를 사용하지
않는다. 이 Actor/STREAM 실패는 이번 readiness 수정 범위 밖으로 남겼다. Timeout·retry·assertion과
sample 코드는 변경하지 않았다. 따라서 요청된 전체 unit 0 failures와 sample 7/7은 미충족이다.

변경 파일은 C++ runtime 파일, C++ 회귀 테스트 파일과 이 결과 문서뿐이다.
Core, binding, 보호 문서와 다른 언어는 이 작업에서 수정하지 않았다. Commit은 수행하지 않았다.
