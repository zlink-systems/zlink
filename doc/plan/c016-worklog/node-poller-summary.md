# Node Poller socket-monitor 지원 요약

## 변경

- 공개 계약에 `SocketMonitor` alias와 `Pollable = BaseSocket | SocketMonitor | Timer | number`를 추가하고, `Poller.add/modify/remove`의 monitor overload 및 `PollEvents.source(index)` source identity를 노출했다.
- runtime poller가 monitor native handle을 기존 `pollerAdd/Modify/Remove`로 전달하되 monitor에는 `PollIn`만 허용하고, 그 외 mask는 `ConfigError(ConfigResult.InvalidArgument, EINVAL)`로 반환하게 했다.
- monitor registration에는 completion owner를 만들거나 drain하지 않는다. socket의 기존 `PollCompletion` ownership/drain 경로는 유지했다.
- socket/monitor/timer/FD에 내부 registration token을 사용해 public slot과 source object를 안정적으로 복원한다. native addon 변경은 필요하지 않았다.
- sample의 connection-ready sleep polling helper를 `Poller` readiness 후 `monitor.recv(RecvFlags.DontWait)` drain 방식으로 변경했다.
- README Poller 사용 문장, public typecheck, source-layout gate 및 generated `dist-tools`를 갱신했다.

## 테스트

- 신규 `tests/poller_monitor.test.ts`: DEALER→ROUTER READY, server close 후 DISCONNECTED, DONTWAIT drain, monitor source identity, modify, remove 후 poll 미전달, PollOut/PollCompletion typed InvalidArgument.
- 신규 테스트 5회 연속 통과.
- socket/timer source identity 회귀 assertion을 기존 pair poller 테스트에 추가했다.

## 게이트

- `PATH="$PWD/node_modules/.bin:$PATH" npm run build`: PASS
- `PATH="$PWD/node_modules/.bin:$PATH" ZLINK_CORE_SOURCE=local ./scripts/rebuild_native.sh`: PASS (main tree Core 0.17.0)
- `PATH="$PWD/node_modules/.bin:$PATH" ZLINK_CORE_SOURCE=local npm test`: PASS (모든 `dist-tools/tests/*.test.js`, sample 7/7 포함)
- 변경 후 focused `npm run typecheck` + pair/poller-monitor 테스트 19/19: PASS
- `git diff --check`: PASS

## Runtime 변경 판정

- 소유 계층: monitor readiness와 handle 등록 규칙은 Core poller 소유, Node는 public contract/runtime adapter만 소유.
- spec 조항: `core/doc/spec/core/05-polling.ko.md` §3 `socket monitor` 행 (`POLLIN`, raw socket과 같은 등록 함수, DONTWAIT drain).
- 교차언어 대조: C sample은 monitor handle을 `zlink_poller_add(..., ZLINK_POLLIN)`에 직접 전달하고, C++은 `socket_monitor_t&` add/modify/remove overload를 제공하며, .NET은 monitor one-shot poll 표면을 제공한다. Node는 기존 reusable `Poller` 구조에 같은 계약을 적용했다.
- 변경 분류: A 계약 적응.

## 언어 spec 제안 문장

- KO: `Pollable`은 `BaseSocket | SocketMonitor | Timer | number`이며, `Poller.add/modify/remove`는 `SocketMonitor` overload를 제공한다. Socket monitor에는 `PollEventFlag.PollIn`만 유효하고 다른 readiness mask는 typed `ConfigResult.InvalidArgument`이며, `PollEvents.source(index)`는 readiness를 낸 동일 monitor 객체를 반환한다. readiness 뒤 application은 `monitor.recv(RecvFlags.DontWait)`를 반복해 queue를 drain한다.
- EN: `Pollable` is `BaseSocket | SocketMonitor | Timer | number`, and `Poller.add/modify/remove` provide `SocketMonitor` overloads. Only `PollEventFlag.PollIn` is valid for a socket monitor; any other readiness mask yields typed `ConfigResult.InvalidArgument`, and `PollEvents.source(index)` returns the same monitor object that became ready. After readiness, the application repeatedly calls `monitor.recv(RecvFlags.DontWait)` to drain the queue.

## BLOCKERS

- 없음.
