# C++ Framework 공통 E2E gap matrix

이 문서는 C++ Framework 구현을 공통 E2E 시나리오와 비교할 때 사용하는 유지보수용
matrix다. 공통 시나리오는 새 public API의 근거가 아니며, 계약 근거는 common spec과
언어별 public contract에서 확인한다. `implemented`로 표시하지 않은 항목은 E2E runner와
검증 증거가 완성될 때까지 완료로 판정하지 않는다.

## 현재 조사 결과

2026-08-07 기준 `verify_common_inventory.sh`가 확인한 범위는 14 configuration, 374
scenario다. C++ 구현은 258개 inventory 조건이 열려 있다. 이 수치는 source가 없거나,
feature-map 상태가 incomplete/deferred가 아니거나, runner가 scenario를 실행하지 않는
경우를 함께 포함한다.

| Configuration | Common scenarios | Open condition | Decision |
|---|---:|---:|---|
| RegistryMessaging | 17 | 0 | 유지하고 regression 검증 |
| SpotService | 66 | 26 | public contract와 fixture를 먼저 비교 |
| PubSub | 24 | 27 | subscriber evidence와 reconnect 경로 구현 |
| RegistrationCodec | 12 | 0 | 유지하고 regression 검증 |
| ResilienceLifecycle | 39 | 40 | lifecycle/terminal-once 경로 구현 |
| DiscoveryRegistryHa | 28 | 36 | store outage와 drain 경로 구현 |
| RuntimeMonitoring | 12 | 2 | MON-B1·MON-B2 evidence 보강 |
| AutomaticTurnDispatch | 32 | 20 | turn/worker scenario 구현 |
| ToActorMessaging | 7 | 0 | TA-B3 readiness admission과 transport terminal을 실제 E2E로 검증 |
| SpotActorTransfer | 43 | 46 | relocation/follow scenario 구현 |
| ObservabilityOps | 22 | 22 | metric/drain evidence 구현 |
| ChannelEgressRouting | 16 | 0 | 유지하고 regression 검증 |
| SubmitAdmission | 20 | 14 | admission/disconnect scenario 구현 |
| InstanceSpot | 36 | 25 | owner replacement/lifecycle scenario 구현 |

## 판정 규칙

각 행의 `source-missing`은 파일 이름을 추가하는 것으로 닫지 않는다. 시나리오의
검증 질문을 읽고, public API 호출, owner/lifecycle 순서, negative evidence와 bounded
timeout을 실제 runner에서 확인해야 한다. 구현이 공통 계약과 맞지 않으면 feature-map에
`deferred` 사유를 남기고 별도 설계 gap으로 유지한다.

특히 실패한 E2E를 수정할 때도 먼저 scenario의 expected terminal과 evidence를 확인한다.
TA-B3의 경우 `route_mesh_runtime_t` snapshot이 `not_ready`가 된 사실만으로 완료하지
않는다. 차단 중 request가 actor handler에 전달되지 않고 `unavailable`로 한 번 종료되며,
재연결 뒤 새 request만 성공해야 한다.

RegistryMessaging의 RM-B3도 같은 기준을 적용한다. C++ runner는 provider A의
`ProfileReqStarted` evidence를 확인한 뒤 A를 강제 종료하고, in-flight 결과가 한 번의
공개 terminal로 끝나는지 확인한다. 이후 consumer의 public ClientServer status에서
A가 ready target에서 제외되고 B 하나만 선택 가능한 상태가 된 뒤, 신규 request 20건이
B에서 각각 한 번 처리되는지 확인한다. 강제 종료된 A의 HTTP evidence endpoint를 다시
조회하지 않으며, B에 in-flight marker가 없다는 사실로 자동 replay 부재를 판정한다.

## RuntimeMonitoring MON-A6 구현 판단

MON-A6는 공통 시나리오가 요구하는 Actor·Spot 생성, placement capacity, typed error,
삭제 후 재생성 순서를 public HTTP 경로와 `route_mesh_runtime_t` snapshot으로 확인한다.
첫 구현에서는 Location Store의 active/reserved capacity가 바뀌어도 descriptor 변경
signature가 증가하지 않아 snapshot이 이전 값을 유지했다. C++ runtime은 lifecycle과
descriptor revision뿐 아니라 placement weight, Actor·Spot·activation capacity,
channel weight를 함께 비교하도록 수정했다.

Spot 생성 경로가 실패한 `result_t`를 HTTP 500으로 바꾸면 공통 계약의
`CapacityExceeded` 의미가 사라진다. Service handler는 public result의 error kind를
읽어 `capacity_exceeded`를 409 응답으로 변환한다. E2E fixture는 provider별 작은
capacity를 설정하고, 잘못 선택된 provider의 객체는 public delete/close 경로로
정리한 뒤 capacity와 recovery를 검증한다. 내부 registry나 private member에는
접근하지 않는다.

Location Store가 연결된 runtime에서는 Store 상태가 `ready`가 아닐 때 remote
admission을 허용하지 않는다. Public snapshot이 `degraded`인 동안 stale peer를
새 대상처럼 선택하면 snapshot과 실제 요청 결과가 달라지므로, readiness resolver도
같은 상태를 `false`로 판정한다. Location Store가 설정되지 않은 local-only runtime은
기존 peer readiness 규칙을 유지한다.

RM-A7의 actual-process 검증에서는 Actor global create와 `Find`가 두 process에서
같은 ref로 수렴했지만, 서로 다른 Mesh의 User Spot remote create는 완료되지 않았다.
현재 RouteMesh topology는 peer descriptor의 MeshName이 local MeshName과 다르면
연결을 거부한다. 따라서 다른 Mesh의 endpoint를 단순 peer 목록에 추가하는 방식은
계약을 만족하지 않으며, target Mesh transport를 Framework가 어떤 public 경계로
선택할지 별도 설계가 필요하다. 이 상태에서는 RM-A7을 완료로 판정하지 않는다.

## M6B admission 회귀 조사 결과

초기 `verify_raw_request_survives_remote_admission_race`는 transport
`connection_ready`를 service admission 완료로 해석하고, target을 service pump하기
전에 `source.topology().peer(target)`를 요구했다. 이 전제는 공통 계약과 맞지
않는다. Transport readiness는 physical route 사용 가능성을 나타내며,
`topology().peer()`는 양쪽이 `hello`와 `admit`을 처리한 뒤에만 생성된다.

또한 Node direct request는 현재 Mesh member가 아닌 target으로 제출할 수 없다.
공통 interaction contract의 규칙에 따라 admission 전 호출은 거부되고, admission
완료 뒤에만 request가 제출된다. 따라서 이 실패는 Core readiness 회귀가 아니라
테스트가 service admission 전 request 수락을 요구한 scenario/spec 불일치였다.

테스트 이름을 `verify_node_request_requires_remote_admission`으로 바꾸고 다음을
검증하도록 수정했다.

1. remote admission 전에 Node direct request가 거부된다.
2. 양쪽 runtime이 `hello`와 `admit`을 처리하면 두 topology에 peer가 등록된다.
3. admission 뒤 같은 Node direct request가 정상적으로 reply를 받는다.

수정 뒤 `test_cpp_framework_m6b_runtime` build와 실행이 통과했다. 이 항목에는
pre-admission request queue를 추가하지 않는다. 그런 동작이 필요하면 먼저
readiness, ownership, error 결과를 공통 spec에 추가한 뒤 별도 설계로 구현해야 한다.

## C++ SupportChat와 STREAM runtime 현재 판정 (2026-08-07)

SupportChat의 C++ multi-process runner는 다음 명령으로 다시 build하고 실행했으며
`PASS SupportChat.Cpp`와 `supportchat sample result=passed`를 출력했다.

```bash
ZLINK_CPP_BUILD_DIR=framework/languages/cpp/build/linux-ninja-vcpkg-debug \
  timeout 300s bash framework/languages/cpp/samples/SupportChat/run_sample.sh
```

이 실행에서 Api, Session, Support와 Client process가 실제 public stream·HTTP·channel
경로를 사용했다. authentication, assignment, deferred join, participant push, two-room
MessageSeq, typing one-way, reconnect/re-auth/rejoin, explicit close, closed typing ignore,
idle resume/close와 no-agent WaitingForAgent를 client assertion과 server flow log로 확인했다.
SupportChat application gap은 이 실행 범위에서 닫혔다. 실행별 flow log는
`framework/languages/cpp/samples/SupportChat/logs/flow-*.log`에 남는다.

C++ STREAM runtime은 외부 TCP·TLS·WebSocket 경로를 Asio async operation으로 처리하고,
Core STREAM은 Core socket과 `runtime_wake_timer_t`를 같은 `poller_t`에 등록한다. stop은
fixed 100 ms polling이나 운영체제별 socket close에 의존하지 않고 timer event로 관찰한다.
connection별 write queue는 active write에 부여한 `write_id`와 connection identity를
completion에서 다시 확인한다. 따라서 late cancellation callback이 이미 끝난 write를
다시 완료하거나 다음 write를 중복 시작하지 않는다. 이 내용은 공통
runtime internals의 wake, progress isolation, session teardown 규칙과 일치하도록 반영했다.

다음 항목은 여전히 별도 gap이다.

- 공통 inventory 전체는 위의 2026-08-07 집계처럼 258개 open condition을 포함한다. 이번
  SupportChat 결과를 전체 C++ E2E 완료로 확대하지 않는다.
- C++ sample smoke target은 현재 Bingo, TicTacToe, DeliveryDispatch, GameQuest,
  ShoppingMall, SupportChat의 6개다. C++ ZoneWorld target과 process runner는 원본
  C++ sample에도 없으므로 구현 gap으로 유지한다.
- 기본 `framework/languages/cpp/build` configure는 dependency prefix 또는 vcpkg
  toolchain이 없으면 `protobufConfig.cmake`를 찾지 못할 수 있다. 명시된
  `linux-ninja-vcpkg-debug` build는 통과했으므로, 남은 항목은 runtime 동작이 아니라
  clean clone에서 dependency provenance를 자동으로 선택하는 build/package gap이다.

## Unreal adapter와 Asio 검증 (2026-08-07)

Unreal adapter가 사용하는 C++ core target은 `zlink_unreal_stream_connector`와 같은
`zlink::stream_connector` static library다. 따라서 TCP·TLS·WebSocket의 socket 동작은
Unreal API가 아니라 core 내부의 Boost.Asio가 소유한다. Unreal public header에는 Asio,
Beast, OpenSSL 타입이 노출되지 않으며, callback은 `manual` dispatch queue를 거쳐
Game Thread의 `Dispatch()`에서 delegate로 전달된다.

Windows에서 필요한 Winsock system library는 runtime source branch가 아니라 CMake link
dependency로 등록했다. `zlink_framework_cpp_add_asio_system_libraries()`가 connector,
framework, HTTP client와 Unreal static target에 `ws2_32`와 `mswsock`을 연결한다.
readable timeout은 socket 전체에 `cancel()`을 호출하지 않고 readiness
`async_wait`에 연결한 `cancellation_signal`만 취소한다. timer와 socket completion은
connection strand에서 직렬화되므로 진행 중인 read/write를 timeout이 취소하지 않는다.

현재 Linux build에서 다음 CMake target과 CTest를 확인했다.

```bash
cmake --build framework/languages/cpp/build/linux-ninja-vcpkg-debug \
  --parallel 2 --target zlink_stream_connector \
  zlink_unreal_stream_connector test_unreal_stream_connector
ctest --test-dir framework/languages/cpp/build/linux-ninja-vcpkg-debug \
  --output-on-failure --parallel 2 \
  -R '^(test_unreal_stream_connector|test_cpp_stream_connector)$'
```

두 test가 모두 통과했고, Unreal smoke는 세 번 반복해 모두 통과했다. 다만 이 CTest는
`CoreMinimal.h`가 없는 standalone fallback wrapper를 compile하고 실행한다. 현재 환경에는
`UnrealBuildTool`과 `UnrealEditor-Cmd`가 없어 UnrealHeaderTool, 실제 UBT link, Editor
Automation Test까지는 실행하지 못했다. 따라서 Asio core의 Linux compile/smoke와
Unreal wrapper contract는 확인했지만, Windows Unreal binary 실행까지 완료했다고 판정하지
않는다.

source plugin packaging은 `Tools/package-third-party.cmake`와
`zlink-unreal-package.manifest` 경로로 정리했다. configured CMake build에서 이 script를
실행하면 Unreal adapter, C++ binding, Core runtime과 선택된 native dependency가
`ThirdParty/ZLink/`에 staging된다. script는 `StreamConnector` install component만
사용하므로 server framework와 HTTP client 산출물을 섞지 않는다. manifest에는
platform, architecture, configuration, compiler와 C++ standard도 기록하며,
`ZLinkStreamConnector.Build.cs`는 이를 검증한 뒤 Unreal module에 include, link, runtime과
system dependency를 등록한다. CMake export는 LZ4 경로를 package prefix 기준으로
재작성하고 Windows Core import library도 함께 설치한다. 따라서 `.uplugin` source만
복사하는 방식의 clean-consumer 누락은 수정했다.

현재 환경에는 UnrealBuildTool, UnrealHeaderTool과 Windows toolchain이 없으므로 실제
Windows Unreal binary와 Editor Automation Test 실행은 별도 검증 조건으로 남는다. 이는
Asio runtime 또는 package manifest 계약의 실패가 아니라 해당 build 환경이 없는 상태다.
