# Java Poller socket-monitor 지원 요약

## 변경

- `Poller`에 기존 socket API와 같은 형태의 monitor overload를 추가했다: `add(SocketMonitor, long, PollEventFlags...)`, `modify(SocketMonitor, PollEventFlags...)`, `remove(SocketMonitor)`.
- `NativePoller`가 monitor native handle을 Core의 `zlink_poller_add/modify/remove` 경로에 전달하고, monitor source를 poller 수명 동안 유지하도록 했다.
- monitor event mask는 `POLLIN` 또는 빈 mask만 허용한다. `POLLOUT`/`POLLCOMPLETION` 등은 `ZlinkConfigException(ConfigResult.NOT_SUPPORTED)`로 거부하며 completion owner 로직은 monitor 경로에서 호출하지 않는다.
- monitor readiness는 기존 socket과 같은 `PollSourceKind.SOCKET` 및 등록 slot으로 `PollEvent`에 보고된다.
- `SampleSupport`의 monitor polling/sleep 및 blocking recv 대기를 `Poller.wait` 후 `recv(DONT_WAIT)` 방식으로 교체했다.
- `MonitorPollingContractTest`를 추가해 DEALER→ROUTER의 inproc/tcp `CONNECTION_READY`, server close 후 `DISCONNECTED`, modify 비활성/재활성, remove 후 미전달, unsupported event typed 오류를 검증한다.
- 요청 경로 `bindings/java/README.md`는 저장소에 존재하지 않아 실제 Java binding README인 `bindings/java/README.javadoc.md`에 Poller monitor 사용 문장을 추가했다.
- spec 보호 경로는 수정하지 않았다.

변경 파일:

- `bindings/java/src/main/java/systems/zlink/contracts/eventing/Poller.java`
- `bindings/java/src/main/java/systems/zlink/runtime/eventing/NativePoller.java`
- `bindings/java/src/main/java/systems/zlink/runtime/eventing/NativeMonitorSocket.java`
- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/InternalAccess.java`
- `bindings/java/src/test/java/systems/zlink/contract/MonitorPollingContractTest.java`
- `bindings/java/samples/Zlink.Samples/src/main/java/systems/zlink/samples/SampleSupport.java`
- `bindings/java/README.javadoc.md`

## 테스트

- 신규 `MonitorPollingContractTest`: 총 5회 반복, 매회 3/3 통과.
- `bindings/java/tests/run_tests.sh`: 전체 통과.
- unit `:test`: 통과.
- integration `:integrationTest`: 통과.
- Netty extension `:zlink-ext-netty:test`: 통과.
- Kotlin contract `:kotlin-contract-test:test`: 통과.
- sample smoke: 7/7 통과.
- `git diff --check`: 통과.
- JDK: `/home/hep7hep7/.jdks/jdk-22.0.2+9`; Gradle workers: 2; Core: main tree `core/build` 0.17.0.

## Runtime 변경 판정

- 소유 계층: poller source의 native 등록/readiness는 Core, Java public overload·handle adaptation·typed surface는 Java binding이 소유한다.
- spec 조항: Core `05-polling` §3의 `socket monitor` 행 — monitor handle은 raw socket과 같은 poller 함수로 등록하고 `POLLIN` 뒤 `zlink_socket_monitor_recv(..., DONTWAIT)`로 drain한다.
- 교차언어 대조: C++ `poller_t::add/modify/remove(socket_monitor_t&)`처럼 native monitor handle을 socket poller 함수에 직접 전달하며, Java도 monitor에 socket completion owner를 적용하지 않는다. .NET one-shot monitor poll도 ready 뒤 monitor receive를 수행한다.
- 변경 분류: **A 계약 적응** — 공통 binding 요구를 Java의 기존 slot+varargs Poller 형태에 추가했다.

## 언어 spec 제안 문장

KO: `Poller`는 `void add(SocketMonitor monitor, long slot, PollEventFlags... events)`, `void modify(SocketMonitor monitor, PollEventFlags... events)`, `boolean remove(SocketMonitor monitor)`를 제공한다. Monitor에는 `POLLIN`만 등록할 수 있고 ready event는 socket source와 같은 `PollSourceKind.SOCKET` 및 등록 slot으로 식별하며, 호출자는 readiness 뒤 `monitor.recv(RecvFlags.DONT_WAIT)`를 `NO_DATA`까지 drain해야 한다.

EN: `Poller` provides `void add(SocketMonitor monitor, long slot, PollEventFlags... events)`, `void modify(SocketMonitor monitor, PollEventFlags... events)`, and `boolean remove(SocketMonitor monitor)`. Only `POLLIN` may be registered for a monitor; a ready event identifies it as `PollSourceKind.SOCKET` with the registered slot, and the caller must drain `monitor.recv(RecvFlags.DONT_WAIT)` until `NO_DATA` after readiness.

## BLOCKERS

- 없음.
