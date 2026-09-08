# Node secure transport 64 KiB SENDSEND echo drain 수정

## 결론

Core 0.17.4에서 Node `MULTI_ROUTER_ROUTER_SENDSEND`와
`MULTI_DEALER_ROUTER_SENDSEND`의 64 KiB echo가 남은 원인은 Core나 binding의
frame 손실이 아니다. Node relay가 reply admission을 기다리는 동안에도 Core receive
queue를 비워 application FIFO에 옮겼고, managed send completion은 public `POLLIN`
poller와 binding runtime watcher가 같은 socket notification source를 나눠 관찰했다.
secure transport의 backpressure 경계에서 FIFO head admission 진행이 멈추면서 client가
이미 admission한 echo가 relay FIFO에 남았다.

변경 분류는 **A — Node perf relay 구조 결함**이다. Core와 Node binding library는
수정하지 않았다.

## 재현과 위치 확정

모든 측정은 다음 고정 Core와 기본 조건을 사용했다.

```text
ZLINK_CORE_SOURCE=release
ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core/0.17.4/linux-x64
clients=100, duration=5s, msg-size=65536, runs=1, auto-HWM
echo drain=15s, server shutdown=5s
```

WSS RR 단일 cell을 두 번 실행한 결과는 모두 `rc=1`이었다.

| 실행 | 제출 | admission 거절 | active echo | active 뒤 0~5초 echo | 이후 10초 echo | 종료 시 미수신 echo |
|---|---:|---:|---:|---:|---:|---:|
| `node64k_diag_wss1` | 45,955 | 0 | 30,029 | 10,670 | 0 | 5,256 |
| `node64k_diag_wss2` | 46,190 | 0 | 30,840 | 9,706 | 0 | 5,644 |

세 번째 server 계수 실행에서는 20초 시점에 client 미수신 4,643건이
`server FIFO 4,642건 + 아직 server가 받지 못한 client admission 1건`과 정확히
일치했다. Server의 reply admission은 5초 시점의 40,025건에서 20초까지 한 건도
늘지 않았다. Echo는 client 수신 경로나 wire에서 사라진 것이 아니라 relay가 소유한
FIFO head 뒤에 남아 있었다.

TCP 대조는 제출 140,743건과 echo 140,743건이 모두 끝나 `rc=0`, `complete`였다.

진단 report:

- WSS 재현 1: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260909_055914_node64k_diag_wss1.txt`
- WSS 재현 2: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260909_055938_node64k_diag_wss2.txt`
- WSS server 계수: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260909_062824_node64k_diag_wss_progress.txt`
- TCP 대조: `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260909_055947_node64k_diag_tcp.txt`

원인 위치는 다음과 같다.

- `bindings/node/perf/multi/perf_multi_routed_sendsend.ts:380,511`: 수정 전 전 client와
  server public poller가 `POLLIN`만 소유했다. Managed send가 backpressure되면 binding
  runtime watcher가 별도로 completion을 기다렸다.
- `bindings/node/src/zlink/runtime/eventing/poller.ts:237-291`(읽기 전용 대조): public
  poller는 `PollCompletion`을 등록할 때만 completion owner를 넘겨받는다.
- `bindings/node/perf/multi/perf_multi_routed_sendsend.ts:546-568`: 수정 전 relay는
  sender가 pending이어도 `recv(DontWait)`를 계속 호출해 Core의 bounded receive queue를
  application FIFO로 옮겼다.

## 수정

- `bindings/node/perf/multi/perf_multi_runtime.ts:15-77`에 public
  `PollCompletion` mask 변환을 추가했다.
- `bindings/node/perf/multi/perf_multi_routed_sendsend.ts:378-460`에서 echo client의 한
  public poller가 `POLLIN|POLLCOMPLETION`을 함께 소유한다. 같은 poller wait가 echo와
  managed send completion을 모두 진행한다.
- `bindings/node/perf/multi/perf_multi_routed_sendsend.ts:511-571`에서 relay poller도
  `POLLIN|POLLCOMPLETION`을 함께 소유한다. 앞 reply가 admission되기 전에는 다음
  request를 Core receive queue에서 꺼내지 않는다. Pending 중에는 정책의 completion
  진행 규칙대로 turn마다 zero-time poll을 한 번 호출하고 기존 `setImmediate` yield를
  수행한다.
- STOP은 `AbortSignal` 하나로 관찰하고, STOP 뒤 남은 reply는 기존
  `relayShutdownDrainMs()`의 3초 bounded drain으로 넘긴다. Timeout, sleep, client 수,
  payload 크기, HWM과 drain 창은 바꾸지 않았다.
- `bindings/node/tests/perf_multi_routed_sendsend_contract.test.ts:367-421`에 STOP이 pending
  reply를 버리지 않고 receive 결합을 중단하는 경우와 public completion 진행이 pending
  admission을 끝내는 경우를 추가했다.

수정 전/후 규칙 수: `Core receive queue + application FIFO + 별도 runtime watcher`의
세 소유자에서 `POLLIN과 completion을 함께 맡는 public poller + Core receive queue`의
두 소유자로 줄었다. 숫자 기반 in-flight cap, 별도 poller, retry 횟수나 새 timer는 없다.

## 교차언어 대조

- C relay는 `bindings/c/perf/multi/common/perf_multi_relay_server.hpp:446-469`에서 한
  poller에 `ZLINK_POLLIN|ZLINK_POLLCOMPLETION`을 등록하고, pending reply가 있으면
  `POLLIN`을 빼 Core receive queue에 backpressure를 남긴다. `:502-521`에서 같은
  poller로 completion을 비운 뒤에만 다음 request를 받는다.
- Java relay는
  `bindings/java/perf/multi/Zlink.BindingBench.Multi/src/main/java/systems/zlink/perf/multi/PerfMultiRoutedRelay.java:73-110`
  에서 `awaitIdle` 뒤에만 다음 request를 받는다.
- Node는 Promise 기반 managed retry이므로 C의 native wait token을 직접 다루지 않는다.
  대신 public `PollCompletion` owner가 같은 역할을 맡고 zero-time poll + cooperative
  yield로 Promise를 진행한다. 언어 구조 차이는 이 지점뿐이며 receive/admission 소유권은
  C와 같다.

## 검증

관련 계약 테스트는 다음 명령으로 14/14 통과했다.

```text
npx tsc -p tsconfig.tools.json
node --test dist-tools/tests/perf_multi_routed_sendsend_contract.test.js
```

첫 수정에서 receive-admission 결합만 적용하고 `PollCompletion` ownership을 빠뜨렸을
때는 DR 4개와 RR ws/wss는 통과했지만 RR tcp/tls에 각각 1건/3건의 admission과 echo가
함께 남았다. 이 측정 실패로 public completion ownership을 추가했고, 같은 조건의 최종
측정은 모두 통과했다.

| 티켓 | 범위 | rc | report | 결과 |
|---|---|---:|---|---|
| `1-1788905023-10529-codex-node64k-node64k_final2_tls_ws_wss_DR_RR_SENDSEND` | tls, ws, wss × DR, RR | 0 | `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260909_072824_node64k_final2_secure.txt` | success 6, fail 0, `complete` |
| `1-1788905023-10533-codex-node64k-node64k_final2_tcp_DR_RR_SENDSEND_65536_` | tcp × DR, RR | 0 | `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260909_072844_node64k_final2_tcp.txt` | success 2, fail 0, `complete` |

두 티켓 log 첫 줄에서
`/home/hep7/.cache/zlink/core/0.17.4/linux-x64/lib/libzlink.so.0.17.4`를 확인했다.
임시 진단 출력은 최종 코드에서 제거했다.
