# Java task 5 결과

## 결과표

| 항목 | 결과 | 근거 |
|---|---|---|
| Java monitor ABI 매핑 | 수정 완료 | Core 공개 event는 800 bytes이며 `connection_id=784`, `transport_lane=792`, `flags=796`이다. Java에 남아 있던 두 개의 obsolete `uint64` field를 제거했다. |
| TCP DEALER→ROUTER identity | 통과 | READY와 DISCONNECTED가 같은 nonzero `connectionId`와 Application lane(ABI 값 0)을 주고, 자동 재연결의 READY는 다른 `connectionId`를 준다. |
| inproc DEALER→ROUTER identity | Core blocker | 실제 관찰값은 READY `connectionId=6`, DISCONNECTED `connectionId=5`였다. 같은 endpoint에 ROUTER를 다시 bind해도 5초 안에 새 READY edge가 없었다. |
| CLOSED identity | Core blocker | TCP는 READY `connectionId=9` 뒤 CLOSED `connectionId=15`를 주었다. inproc peer close에서는 5초 안에 CLOSED가 없었다. |
| Java Poller의 monitor 등록 | spec gap, 미구현 | Java spec은 raw socket/FD readiness만 요구하며 `SocketMonitor` 등록 signature를 정의하지 않는다. 지시대로 public API를 추가하지 않았다. |
| 보호 범위 | 준수 | `bindings/java/**`만 변경했다. spec과 `framework/**`는 읽기만 했다. |

`transport_lane`의 Application 값은 Core ABI에서 0이다. 따라서 “nonzero identity”는 `connectionId != 0`으로 판정하고, lane은 두 event에서 같은 유효 enum 값인지 확인했다.

## 적용한 spec 조항

- `core/doc/spec/core/06-monitoring.ko.md:67-72`: “`connection_id`는 현재 프로세스에서 하나의 물리적 transport 시도를 식별”하며 `transport_lane`은 physical connection을 분류한다. DEALER-ROUTER의 physical event는 Application lane이다.
- `core/doc/spec/core/06-monitoring.ko.md:74-77`: DEALER-ROUTER의 CONNECTION_READY는 logical peer 하나로 집계한다.
- `bindings/doc/spec/java/README.ko.md:1088-1092`: Monitor는 `recv`/`status`/`close`를 제공하며 `connectionId`는 진단과 correlation에만 쓴다.
- `bindings/doc/spec/java/README.ko.md:175-185`: Java runtime은 `Poller`로 ready socket을 기다린 뒤 socket을 DONT_WAIT로 drain한다. monitor 등록 계약은 없다.
- `bindings/doc/spec/README.ko.md:4762-4765`: Poller conditional test는 raw socket 또는 FD readiness를 요구한다. monitor readiness 등록은 요구하지 않는다.

## 원인

### Binding

- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/NativeLayouts.java:249-257`: Java layout에 public Core struct에는 없는 `reserved_pair_id`, `reserved_pair_epoch`가 들어가 있었다. 결과적으로 Java가 `transport_lane`과 `flags`를 각각 808, 812 offset에서 읽었다.
- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/Native.java:1203-1208`: `monitorRecv`는 위 offset을 그대로 사용하므로 `connectionId`만 우연히 맞고 lane/READY edge flag는 잘못 읽었다.

### Core blockers

- `core/src/runtime/sockets/common/socket_base_endpoint.cpp:338-341`: inproc pipe 양쪽에 connect/bind endpoint pair를 따로 만든다.
- `core/src/runtime/core/endpoint.cpp:25-33`: endpoint pair를 만들 때마다 새 `connection_id`를 발급한다. 이 경로 때문에 inproc READY와 DISCONNECTED가 서로 다른 ID를 관찰했다.
- `core/src/runtime/transports/tcp/asio_tcp_connecter.cpp:332-338`: TCP CLOSED를 만들 때 `make_unconnected_connect_endpoint_pair`로 새 endpoint identity를 만든다.
- `core/src/runtime/core/endpoint.cpp:84-92`, `core/src/runtime/sockets/common/socket_base_monitor.cpp:435-440`: 새 ID가 CLOSED event에 그대로 전달된다.
- inproc path에는 TCP/IPC/WS/TLS transport의 `event_closed`와 동등한 호출이 없다. 실제 테스트에서도 peer close 뒤 CLOSED가 오지 않았다.

## 변경

- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/NativeLayouts.java`: monitor event layout을 Core 공개 ABI와 같은 800 bytes로 수정했다.
- `bindings/java/src/test/java/systems/zlink/runtime/nativeapi/NativeLayoutsTest.java`: 크기와 세 tail offset을 고정했다.
- `bindings/java/src/test/java/systems/zlink/contract/MonitorConnectionIdentityContractTest.java`: TCP READY→DISCONNECTED→자동 재연결 identity 계약을 추가했다. Core가 아직 만족하지 않는 inproc/CLOSED 계약 3개는 원인과 함께 `@Disabled`로 고정했다.

## Poller 표면 비교

| binding | monitor readiness 표면 | 위치 |
|---|---|---|
| Java 현재 | 없음. `add(Socket)`, `addFd`, `add(ZlinkTimer)`만 제공 | `bindings/java/src/main/java/systems/zlink/contracts/eventing/Poller.java:12-34` |
| C++ | `add/modify/remove(socket_monitor_t&)` | `bindings/cpp/include/zlink/Contracts/Eventing/poller.hpp:36-46` |
| .NET | `ZlinkPoll.Poll(IReadOnlyList<ISocketMonitor>, ...)` | `bindings/dotnet/src/Zlink/Contracts/Eventing/ZlinkPoll.cs:35-57` |
| Java spec | monitor 등록 signature 없음 | `bindings/doc/spec/java/README.ko.md:175-185`, `1088-1092` |

## 테스트와 gate

| 실행 | 결과 |
|---|---|
| `monitorEventAllocationMatchesCoreEventSize` | PASS |
| `tcpReadyAndDisconnectedKeepIdentityAcrossReconnect` | PASS |
| 새 identity test 반복 | 5/5 PASS, 매회 active 1 / Core-blocked skipped 3 |
| `bindings/java/tests/run_tests.sh` `:test` | 97 tests, 0 failures, 0 errors, 3 skipped |
| `:integrationTest` | 17 tests, 0 failures |
| `:zlink-ext-netty:test` | 3 tests, 0 failures |
| `:kotlin-contract-test:test` | 4 tests, 0 failures |
| sample smoke | 7/7 PASS (`runMonitorRecv` 포함) |
| `git diff --check` | PASS |

## BLOCKERS

1. Core: inproc READY와 DISCONNECTED의 `connection_id`가 다르며, 서버 재생성 뒤 자동 재연결 READY edge도 오지 않는다.
2. Core: TCP CLOSED는 연결의 READY/DISCONNECTED ID 대신 새 endpoint ID를 쓴다. inproc은 peer close에서 CLOSED를 방출하지 않는다.
3. spec gap — 사용자 결정 필요: Java `Poller`에 `SocketMonitor` readiness 등록 표면을 추가할지 결정해야 한다. C++과 .NET에는 동등 기능이 있으나 Java spec에는 signature와 ownership/lifecycle 규칙이 없다.

binding 버그 → 수정(bindings/java/src/main/java/systems/zlink/runtime/nativeapi/NativeLayouts.java)
