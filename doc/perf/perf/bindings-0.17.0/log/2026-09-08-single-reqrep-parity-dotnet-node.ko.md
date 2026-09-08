# Single REQREP 러너 C 모델 정합 — .NET / Node (D-BP40 admission window)

- 일자: 2026-09-08
- branch: `main`
- Core source: `release`
- Core package(before 측정): `/home/hep7/.cache/zlink/core-pinned/0.17.3-alpha`
  (`lib/libzlink.so.0.17.2`, SHA-256 `c4f4da4bb06573837d5945ccc489d6e940610a5e371885c154fd17409112fb39`)
- Core package(after 측정): **`/home/hep7/.cache/zlink/core-pinned/0.17.3`**
  (`lib/libzlink.so.0.17.3`, SHA-256 `4779e33380356d469ec3b0c30b12bb6c828c8710a4cdd71b44d0669cafa1a1af`)
  — VERSION 0.17.3 bump 뒤 alpha prefix(manifest 0.17.2)가 버전 검사로 모든 러너를
  즉시 실패시켜, 감독자가 VERSION과 일치하는 `core/v0.17.3` prefix를 빌드해 재고정했다
  (D-BP42, § 7).
- 변경 파일: `bindings/dotnet/perf/single/Zlink.BindingBench/src/PerfReqRep.cs`,
  `bindings/node/perf/single/perf_socket_reqrep.ts` (그 외 없음)
- 기준 측정(before): `sg1` 태그 1-run
  - C `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_130119_sg1.txt`
  - .NET `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260908_132034_sg1.txt`
  - Node `bindings/node/perf/results/single/report/perf_node_single_linux_20260908_133711_sg1.txt`
- 검증(after): tcp, 5 s, 1 run, C 짝지음(태그 동일), 모두 `status: complete` / `fail: 0`
  - `sgfix-dotnet` C `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_160754_sgfix-dotnet.txt`
    / .NET `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260908_160856_sgfix-dotnet.txt`
  - `sgfix-node` C `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_162849_sgfix-node.txt`
    / Node `bindings/node/perf/results/single/report/perf_node_single_linux_20260908_163103_sgfix-node.txt`

## 1. 무엇이 잘못돼 있었나

C 기준(`bindings/c/perf/single/common/perf_single_reqrep.hpp` `run_request_phase`
396-443)은 한 turn에서 **`ZLINK_SUBMIT_BACKPRESSURED`가 나올 때까지 연속 제출**하고,
64건마다 `poll_completion_once(0)`으로 completion을 회수한 뒤, 경계에 닿았을 때만
`poll(50)`으로 블로킹한다.

- **.NET** (`PerfReqRep.cs:498-521`, 변경 전): turn마다 1건 제출 → `ProgressOnce(50)`.
  이 wait는 미완료가 1건이면 그 1건의 reply가 도착할 때 반환한다. 즉 turn = 1 제출 +
  1 왕복이고, 정책 § 1.1이 금지한 1:1 ping-pong이다.
- **Node** (`perf_socket_reqrep.ts:157-165`, 변경 전): turn마다 1건 제출 →
  `wait(events, 0)` + `await sleepImmediate()`. reply gate는 없지만 **제출을 멈추는
  경계도 없다**. event-loop turn 비용이 유일한 제동이라 큰 크기에서 미완료가 계속 쌓인다.

`sg1` 수치에서 미완료 깊이(= throughput × latency, Little's law)를 계산하면 두 증상이
그대로 드러난다.

**DEALER_ROUTER_REQREP** (tcp, 5 s, 1 run)

| size | C tp | C lat(ms) | C 깊이 | .NET tp | lat | 깊이 | %C | Node tp | lat | 깊이 | %C |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 64 | — | — | — | 9,348 | 0.106 | **1.0** | — | 126,044 | 0.148 | 18.6 | — |
| 256 | 834,097 | 0.853 | 712 | 8,867 | 0.112 | **1.0** | 1% | 121,774 | 0.169 | 20.6 | 15% |
| 1024 | 740,419 | 1.316 | 974 | 8,487 | 0.117 | **1.0** | 1% | 108,505 | 0.211 | 22.8 | 15% |
| 65536 | 11,426 | 0.170 | 1.9 | 6,166 | 0.161 | **1.0** | 54% | 1,120 | 158.99 | 178 | 10% |
| 131072 | 9,234 | 0.210 | 1.9 | 5,378 | 0.185 | **1.0** | 58% | 978 | 123.92 | 121 | 11% |
| 262144 | 7,256 | 0.266 | 1.9 | 3,955 | 0.252 | **1.0** | 55% | 863 | 79.06 | 68 | 12% |

**ROUTER_ROUTER_REQREP**

| size | C tp | C lat(ms) | C 깊이 | .NET tp | lat | 깊이 | %C | Node tp | lat | 깊이 | %C |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 64 | 881,818 | 0.177 | 157 | 9,668 | 0.103 | **1.0** | 1% | 114,700 | 0.148 | 17.0 | 13% |
| 256 | 837,156 | 0.172 | 144 | 9,128 | 0.109 | **1.0** | 1% | 108,583 | 0.165 | 17.9 | 13% |
| 1024 | 724,467 | 7.347 | 5322 | 9,228 | 0.108 | **1.0** | 1% | 98,233 | 0.179 | 17.6 | 14% |
| 65536 | 11,258 | 0.172 | 1.9 | 7,373 | 0.135 | **1.0** | 65% | 942 | 196.29 | 185 | 8% |
| 131072 | 9,154 | 0.212 | 1.9 | 5,537 | 0.180 | **1.0** | 60% | 906 | 133.00 | 121 | 10% |
| 262144 | 7,039 | 0.274 | 1.9 | 3,796 | 0.262 | **1.0** | 54% | 742 | 104.59 | 78 | 11% |

- .NET 깊이는 **모든 pattern·모든 크기에서 정확히 1.0**이다. latency가 C의 0.60~0.64x인
  것도 큐 대기가 없기 때문이며, "처리량 1/3, latency 0.6x"의 정체가 이것이다.
- 총계 33.8%/30.6%는 큰 크기 셀(54~65%)이 끌어올린 값이고, 실제 손실은 작은 크기에서
  **1%**다. 그 구간에서 C는 144~5322 깊이로 돈다.
- C도 65536 B 이상에서는 깊이 1.9다. 큰 크기의 남은 35~45% 차이는 깊이가 아니라
  per-op 오버헤드다.
- Node는 반대 방향으로 어긋나 있다. 큰 크기 깊이 68~185, latency 79~196 ms이고
  deadline 이후 완료는 집계에서 빠지므로 처리량이 C의 8~12%까지 떨어진다.

## 2. 차이 표 (C 기준)

| 항목 | C | .NET (before → after) | Node (before → after) |
|---|---|---|---|
| 제출 gate | `BACKPRESSURED`까지 연속 제출 | 1건/turn + reply 대기 → **admission window까지 연속 제출** | 1건/turn, 경계 없음 → **admission window까지 연속 제출** |
| 미완료 상한 | 없음(Core admission 경계) | 실효 1 → **applied SNDHWM bytes ÷ wire size** | 실효 무제한 → **동일** |
| 제출 중 progress | 64건마다 `poll(0)` | 없음 → **64건마다 `Wait(0)`** | 없음 → **64건마다 `wait(0)` + turn 양보** |
| 경계 도달 시 | `poll(50)` 블로킹 | 매 turn `Wait(50)` → **`Wait(submittedAny ? 0 : 50)`** | 항상 `wait(0)` → **`wait(submittedAny ? 0 : 50)`** |
| reply drain | `completion_recv`를 `NO_DATA`까지 | `Poller.Wait` 내부 `Drain()` 전량 + settle | Promise settle |
| 진행 주체 | 전용 requester OS thread | 전용 `Thread` ✓ | 전용 main loop(replier는 worker) ✓ |
| timestamp | 제출 직전 stamp / completion 시각 | 동일 ✓ | 동일 ✓ |
| active 유효 조건 | `completed_at < deadline` + run/phase/size | 동일 ✓ | 동일 ✓ |
| throughput | `completed / duration` | 동일 ✓ | 동일 ✓ |
| request timeout knob | `PERF_SINGLE_REQREP_TIMEOUT_MS`(200) | 동일 ✓ | `PERF_SINGLE_RCVTIMEO_MS`(200) — knob 다름(미변경, § 7) |
| completion drain bound | `PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS`(10000) | 동일 ✓ | `max(1000, timeout*4)` — 다름(미변경, § 7) |

## 3. 수정 (D-BP40)

정책 § 1.1.3(2026-09-08 개정): awaitable이 admission과 reply를 합쳐 제공하는 binding은
미완료 집합을 **Core가 그 소켓에 적용한 SNDHWM bytes ÷ 메시지 wire size**로 묶는다.
고정 숫자 상한이 아니라 C 러너가 `BACKPRESSURED`를 받는 admission 창 자체다.

### 3.1 .NET

- `ResolveAdmissionWindow(requester, appliedSendHwmBytes, wireSize)` 추가.
  값의 출처는 auto-HWM monitor snapshot(`MonitorStatus.AutoHwmAppliedSendHighWaterMarkBytes`)
  이며, 수동 `PERF_SINGLE_SNDHWM` override처럼 snapshot이 0인 경우에만
  `requester.Options.SendHighWaterMark`로 되읽는다. 둘 다 0이면 상한을 임의로 정하지
  않고 예외로 실패시킨다.
- 두 pattern 진입점에서 `EmitSingleAutoHwmDetail` 직후, monitor를 닫기 전에 applied
  SNDHWM을 읽어 `RunRequestLoop`까지 넘긴다.
- requester turn을 C 모양으로 되돌렸다.
  ```csharp
  while (now < deadline && !fatal) {
      bool submittedAny = false; int submittedSinceProgress = 0;
      while (now < deadline && !fatal && pending.Count < admissionWindow) {
          submit(); submittedAny = true;
          if (++submittedSinceProgress >= 64) { submittedSinceProgress = 0; ProgressOnce(0); }
      }
      ProgressOnce(submittedAny ? 0 : 50);   // 창이 찼을 때만 블로킹
  }
  ```
- `SettleCompleted()`를 역방향 `RemoveAt` 제거에서 **전진 compaction 1회 pass**로 바꿨다.
  창이 작은 크기에서 16384까지 열리므로 항목마다 `List.RemoveAt`을 하면 측정 루프 안에
  제곱 비용이 들어간다. settle 본문은 `SettleOne(request)`로 분리했다(로직 동일).
- `ProgressOnce`는 `Poller.Wait`의 반환값이 0보다 클 때만 settle한다. 이 poller가
  socket의 유일한 completion drain owner이므로 Task는 `Wait` 안에서만 완료되고,
  readiness가 없으면 스캔할 것도 없다(`CompletionOwner.DrainCore`는 REQUEST completion
  수를 세어 `Poller.Wait`가 그 경우에만 이벤트를 내보낸다).
- 진단용으로 `single_reqrep_debug:admission_window=...:applied_sndhwm_bytes=...:wire_size=...`
  를 `DebugLog`로 남긴다. RESULT/AUTO_HWM_DETAIL 출력 형식은 바꾸지 않았다(리포트의
  `sndhwm`과 `msg_size`로 창 크기를 그대로 재계산할 수 있다).

### 3.2 Node

- `resolveAdmissionWindow(client, monitor, wireSize)` 추가. `monitor.status().autoHwmAppliedSndHwmBytes`
  → (0이면) `client.options.sendHwm` → 둘 다 0이면 예외. .NET과 같은 계산식이다.
- turn 구조를 .NET·C와 같은 모양으로 바꿨다. 내부 제출 루프는 `pending.size < admissionWindow`
  까지 돌고, 64건마다 `wait(0)` + `await sleepImmediate()`로 Promise를 settle시킨다
  (Node에서 Promise 완료는 loop turn을 한 번 넘겨야 관측된다). 창이 찼고 이번 turn에
  제출한 것이 없으면 `wait(events, 50)`으로 블로킹한다 — C가 backpressure 뒤에 하는
  블로킹 poll과 같은 자리다.
- 측정 구간 진행 주체는 그대로 이 thread다. `wait`는 이 thread가 자기 completion을
  진행시키는 호출이며 다른 scheduler에 넘기는 것이 아니다.

### 3.3 넣지 않은 것

- 고정 숫자 상한, 새 환경 변수, 새 timeout, sleep, client 수·크기·HWM 변경 없음.
- binding 라이브러리(`bindings/dotnet/src`, `bindings/node/src`) 변경 없음.

### 3.4 창 크기 (auto-HWM 1048576 B 기준)

| size(B) | 64 | 256 | 1024 | 65536 | 131072 | 262144 |
|---|---|---|---|---|---|---|
| admission window | 16384 | 4096 | 1024 | 16 | 8 | 4 |
| C 실측 깊이 | 157 | 144/712 | 974 | 1.9 | 1.9 | 1.9 |

1024 B에서 창(1024)과 C 실측 깊이(974)가 사실상 일치한다. 작은 크기에서는 창이 C의
실측 깊이보다 크므로 실제 깊이는 제출·완료 속도 균형이 정한다 — 창은 상한일 뿐이다.
65536 B 이상에서는 창(16/8/4)이 C 깊이(1.9)보다 조금 크다. 검증 때 확인할 지표는
**깊이 = throughput × latency가 C와 같은 자릿수인가**이다.

## 4. before / after (tcp, 5 s, 1 run, 깊이 = throughput × latency)

`%C`는 같은 티켓에서 짝지은 C 리포트 대비 값이다(before는 `sg1`, after는 `sgfix-*`).

### 4.1 .NET

| pattern | size | before tp (깊이) | %C | after tp (깊이) | %C | 배수 |
|---|---|---|---|---|---|---|
| DR_REQREP | 64 | 9,348 (1.0) | — | 423,353 (124.6) | 487%¹ | 45.3x |
| DR_REQREP | 256 | 8,867 (1.0) | 1% | 412,242 (111.4) | 50% | 46.5x |
| DR_REQREP | 1024 | 8,487 (1.0) | 1% | 389,494 (94.6) | 54% | 45.9x |
| DR_REQREP | 65536 | 6,166 (1.0) | 54% | 7,732 (16.0) | 68% | 1.3x |
| DR_REQREP | 131072 | 5,378 (1.0) | 58% | 5,803 (8.0) | 61% | 1.1x |
| DR_REQREP | 262144 | 3,955 (1.0) | 55% | 4,707 (4.0) | 62% | 1.2x |
| RR_REQREP | 64 | 9,668 (1.0) | 1% | 385,053 (96.1) | 44% | 39.8x |
| RR_REQREP | 256 | 9,128 (1.0) | 1% | 383,794 (93.6) | 46% | 42.0x |
| RR_REQREP | 1024 | 9,228 (1.0) | 1% | 354,451 (95.9) | 49% | 38.4x |
| RR_REQREP | 65536 | 7,373 (1.0) | 65% | 7,293 (16.0) | 62% | 1.0x |
| RR_REQREP | 131072 | 5,537 (1.0) | 60% | 5,788 (8.0) | 60% | 1.0x |
| RR_REQREP | 262144 | 3,796 (1.0) | 54% | 4,415 (4.0) | 58% | 1.2x |

¹ 짝 C run의 DR 64 B 셀이 86,990 ops/s·latency 11.0 ms로 흔들렸다(다른 run은 ~880k). C
측 outlier이므로 이 셀의 `%C`는 쓰지 않는다(§ 6.2).

### 4.2 Node

| pattern | size | before tp (깊이) | %C | after tp (깊이) | %C | 배수 |
|---|---|---|---|---|---|---|
| DR_REQREP | 64 | 126,044 (18.6) | — | 177,768 (122.0) | 22%² | 1.4x |
| DR_REQREP | 256 | 121,774 (20.6) | 15% | 162,677 (153.4) | 20% | 1.3x |
| DR_REQREP | 1024 | 108,505 (22.8) | 15% | 142,756 (178.6) | 20% | 1.3x |
| DR_REQREP | 65536 | 1,120 (178.1) | 10% | 4,939 (15.9) | 44% | 4.4x |
| DR_REQREP | 131072 | 978 (121.2) | 11% | 4,027 (7.9) | 44% | 4.1x |
| DR_REQREP | 262144 | 863 (68.2) | 12% | 2,798 (3.9) | 40% | 3.2x |
| RR_REQREP | 64 | 114,700 (17.0) | 13% | 167,593 (105.8) | 19% | 1.5x |
| RR_REQREP | 256 | 108,583 (17.9) | 13% | 152,923 (126.3) | 19% | 1.4x |
| RR_REQREP | 1024 | 98,233 (17.6) | 14% | 131,508 (146.2) | 19% | 1.3x |
| RR_REQREP | 65536 | 942 (184.9) | 8% | 4,253 (15.9) | 36% | 4.5x |
| RR_REQREP | 131072 | 906 (120.5) | 10% | 3,441 (8.0) | 37% | 3.8x |
| RR_REQREP | 262144 | 742 (77.6) | 11% | 2,691 (3.9) | 35% | 3.6x |

² 짝 C run의 DR 64 B가 791,221 ops/s·9.4 ms(깊이 7459)로 흔들렸다.

### 4.3 깊이 판정

| size | admission window | C 깊이(대표) | .NET 깊이 | Node 깊이 |
|---|---|---|---|---|
| 64 | 16384 | 125~157 | 96~125 | 106~122 |
| 256 | 4096 | 166~484 | 94~111 | 126~153 |
| 1024 | 1024 | 1020~1026 | 95~96 | 146~179 |
| 65536 | 16 | 1.9 | 16.0 | 15.9 |
| 131072 | 8 | 1.9 | 8.0 | 7.9 |
| 262144 | 4 | 1.9 | 4.0 | 3.9 |

- 작은 크기(64~1024 B): 깊이가 1.0(.NET)·17~23(Node)에서 **94~179**로 올라와 C(125~1026)와
  같은 자릿수가 됐다. 창(16384/4096/1024)에 닿지 않고 제출·완료 속도 균형이 깊이를 정한
  구간이며, 이것이 C와 같은 동작이다.
- 큰 크기(≥65536 B): 두 언어 모두 깊이가 창 값(16/8/4)에 **정확히** 고정됐다. 창이
  Core의 실제 admission 경계(C 깊이 1.9)보다 한 자릿수 깊다는 뜻이며, 그 대가로 latency가
  C의 0.17 ms 대비 .NET 2.07 ms / Node 3.23 ms로 늘고 처리량은 오히려 올랐다
  (.NET 54%→68%, Node 10%→44%). 이 구간의 창은 "byte HWM ÷ wire size"가 Core 내부의
  실제 경계(프레이밍·peer 큐 단위)보다 크게 나오는 경우다. 더 좁히려면 러너가 아니라
  binding request terminal이 admission 결과를 노출해야 한다.

## 5. 빌드

- .NET: `dotnet build perf/single/Zlink.BindingBench/Zlink.BindingBench.csproj -c Release`
  → 0 warning, 0 error.
- Node: `npm run build:incremental` 성공, `npm run typecheck` 성공.
  `dist-tools/perf/single/perf_socket_reqrep.js`에 변경이 반영됐다.

## 6. .NET PUBSUB 175% — 수신 count 방식은 C와 동일하다

`PerfPubSub.cs:86-192` ↔ `bindings/c/perf/single/src/perf_pubsub.cpp` 대조.

| 항목 | C | .NET |
|---|---|---|
| topic | `"bench"` 발행 / 빈 prefix 구독 후 topic 비교 | 동일 |
| drop 정책 | `ZLINK_PUB_OPT_NODROP` 기본 1 (`PERF_SINGLE_PUBSUB_XPUB_NODROP=0`일 때만 0) | 동일 (`SocketOption.XPubNoDrop = 0x3305`, 같은 값·같은 시점(bind 전) 설정) |
| backpressure | publish 거절 → **1 ms sleep** → 새 timestamp로 재stamp 후 재제출 | 동일 (`PublishActiveMessageBlocking` false → `Thread.Sleep(1)` → 루프 상단에서 재stamp) |
| part 수 | payload + 빈 tail 2 part | 동일 (`PerfSocketIo.PublishMeasurement`) |
| 유효 메시지 | frame 크기 == payload_size, header decode, run/phase/size 일치 | 동일 (`TryDecodeExpectedSingleHeader`가 길이까지 검사) |
| active 창 | `recv 시각 < active deadline`만 count | 동일 (`recvTicks < deadlineTicks`) |
| throughput | `received / duration` | 동일 |
| auto-HWM | pub 1048576 / sub 2097152 | 리포트상 **동일** |

즉 **drop 여부·창 경계·집계식 어디에도 차이가 없다.** 175%는 집계 artifact가 아니다.

원인 위치는 각 언어의 one-way 천장과 비교하면 드러난다(65536 B tcp).

| 65536 B | PAIR | DEALER_DEALER | DEALER_ROUTER | ROUTER_ROUTER | **PUBSUB** |
|---|---|---|---|---|---|
| C | 107,591 | 110,502 | 109,247 | 107,241 | **13,631** |
| .NET | 99,066 | 100,686 | 106,186 | 104,908 | **44,259** |
| .NET/C | 92% | 91% | 97% | 98% | **325%** |

one-way 4개는 92~98% parity인데 PUBSUB만 두 언어 모두 자기 천장의 13%·41%로 떨어진다.
공통 원인은 NODROP publisher가 HWM에서 거절될 때마다 정책 § 1.1이 요구하는 **1 ms
sleep**을 하는 것이다. 이 구간의 처리량은 전송 속도가 아니라 "얼마나 자주 1 ms를
버리는가"가 결정하며, publisher가 빠를수록 HWM에 더 자주 닿아 더 많이 벌점을 받는
역설적 구조다(작은 크기에서는 C가 .NET보다 빠르다 — 64 B에서 C 1,452,032 vs .NET
896,113, 62%). 따라서 PUBSUB 대형 셀은 현재 **retry cadence 측정**에 가깝다.

다음 단계 제안(이번 과제 범위 밖): publisher 거절 횟수를 debug 카운터로 남겨 C와 .NET을
같은 조건에서 비교하면 이 가설을 바로 확정할 수 있다.

## 7. 검증 이력 — VERSION 0.17.3 bump와 재실행

14:00 `0761c1d4d0 chore(version): bump libzlink and bindings to 0.17.3` 이후,
`bindings/tools/local_core_runtime.sh:38-42`가 고정 prefix의 provenance manifest
버전(0.17.2)과 저장소 VERSION(0.17.3)을 비교해 즉시 실패한다.

```
Core release prefix version 0.17.2 does not match 0.17.3
```

이 실패는 러너 코드와 무관하며 이 시각 이후 모든 언어에서 동일하다(감독자의 p1
`s173x STREAM ws,wss,tls node` 티켓도 같은 한 줄로 rc=1). 감독자 지시에 따라 측정을
멈췄고, 제출해 둔 티켓 중 pending 상태였던 smoke2는 회수했다.

| 티켓 | 결과 |
|---|---|
| `smoke D-BP40 admission window dotnet DR REQREP tcp 64,262144 2s` | rc=1 (version mismatch) |
| `smoke D-BP40 admission window node DR REQREP tcp 64,262144 2s` | rc=1 (version mismatch) |
| `sgfix-dotnet paired C then dotnet DR RR REQREP tcp 6 sizes 1-run` | rc=1 (version mismatch) |
| `sgfix-node paired C then node DR RR REQREP tcp 6 sizes 1-run` | rc=1 (version mismatch) |
| `smoke2 ... (manifest에서 core version 유도)` | 감독자 중단 지시로 회수 |

### 7.1 재실행 (D-BP42, `core-pinned/0.17.3`)

| 티켓 | 결과 |
|---|---|
| `sgfix-dotnet paired ...` (1차) | rc=1 — C `--reuse-build`에 REQREP 바이너리 없음(0.17.3 Single 재빌드가 `DEALER_DEALER`만 빌드) |
| `sgfix-node paired ...` (1차) | rc=1 — 같은 사유 |
| `sgfix-dotnet retry ...` | **rc=0** — C·.NET 모두 `status: complete`, fail 0 |
| `sgfix-node retry ...` | rc=1 — **C 러너 결함**: DR 256 B·1024 B가 `replier_fatal=1 ... received=541495 replied=0 completed=0`으로 FAIL(§ 7.2). `&&` 때문에 Node leg 미실행 |
| `sgfix-node retry2 ...` (두 leg 모두 실행) | **rc=0** — `leg_rc c=0 node=0`, 양쪽 `status: complete`, fail 0 |

### 7.2 관찰된 C 러너 흔들림 (러너 정합과 무관, 보고용)

- `sgfix-node` 1차 C run: `DEALER_ROUTER_REQREP` 256 B·1024 B가
  `[perf-single-reqrep] shutdown failed requester=0 requester_fatal=0 in_flight=0 retained=1
  wait_token=... retry_ready=0 stop=0 poller=1 replier_fatal=1 received=... replied=0 completed=0`
  으로 FAIL. replier가 request를 다 받고도 reply를 한 건도 제출하지 못한 상태다.
- `sgfix-dotnet` C run: DR 64 B가 86,990 ops/s·latency 11.0 ms(깊이 957),
  `sgfix-node` C run: DR 64 B가 791,221 ops/s·9.4 ms(깊이 7459) — 같은 셀이 run마다
  수백~수천 깊이로 흔들린다. 작은 크기 `%C` 비교 시 이 양안정성을 감안해야 한다.

새 prefix로 다시 낼 때 쓴 명령은 아래와 같다.

```
bash scripts/perf/perf-ticket.sh submit -p 2 -o claude-single-netnode \
  -d "sgfix-dotnet paired C then dotnet DR RR REQREP tcp 6 sizes 1-run" -- \
  bash -c 'bash bindings/c/perf/run_benchmarks.sh --reuse-build --pattern DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP --transports tcp --duration 5 --runs 1 --results-tag sgfix-dotnet && bash bindings/dotnet/perf/run_benchmarks.sh --reuse-build --pattern DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP --transports tcp --duration 5 --runs 1 --results-tag sgfix-dotnet'
```
(Node는 같은 형태로 `bindings/node/perf/run_benchmarks.sh`, 태그 `sgfix-node`.)

## 8. 이번에 고치지 않은 잔여 차이

- Node request timeout이 `PERF_SINGLE_RCVTIMEO_MS`를 쓴다(`perf_socket_reqrep.ts:110-113`).
  C는 `PERF_SINGLE_REQREP_TIMEOUT_MS`다. 기본값이 둘 다 200 ms라 지금 측정은 같지만
  `--recv-timeout`을 주면 갈라진다.
- Node completion drain bound가 `max(1000, requestTimeout*4)` 고정이다. C는
  `PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS`(기본 10000)다.
- Go single REQREP은 latency를 `/2.0`으로 나눠 담는다
  (`bindings/go/perf/single/perf_reqrep.go:88`). C는 왕복 전체가 latency다. Go 담당자
  확인 필요.

## 9. 검증 결과 (감독자 기록, `core-pinned/0.17.3`, 1-run, C 짝지음)

§7의 보류는 `core/v0.17.3` prefix 재고정(D-BP42) 뒤 같은 명령을 다시 내 해소했다. 두 언어 모두
`status: complete`, fail 0.

**.NET** (`sgfix-dotnet`, C `perf_c_single_linux_20260908_160754_sgfix-dotnet.txt`)

| pattern | size | before tp (깊이) | after tp (깊이) | %C before→after |
|---|---|---|---|---|
| DR_REQREP | 256 | 8,867 (1.0) | 412,242 (111) | 1% → 50% |
| DR_REQREP | 1024 | 8,487 (1.0) | 389,494 (95) | 1% → 54% |
| DR_REQREP | 65536 | 6,166 (1.0) | 7,732 (16.0) | 54% → 68% |
| RR_REQREP | 64 | 9,668 (1.0) | 385,053 (96) | 1% → 44% |
| RR_REQREP | 1024 | 9,228 (1.0) | 354,451 (96) | 1% → 49% |
| RR_REQREP | 262144 | 3,796 (1.0) | 4,415 (4.0) | 54% → 58% |

집계: DR 33.8% → 58.8%(64 B는 C 러너 결함으로 무효, 5개 평균), RR 30.6% → 52.9%.

**Node** (`sgfix-node`, C `perf_c_single_linux_20260908_162849_sgfix-node.txt`,
Node `perf_node_single_linux_20260908_163103_sgfix-node.txt`)

| pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | 집계 before → after |
|---|---|---|---|---|---|---|---|
| DR_REQREP %C | 22.5 | 19.5 | 19.6 | 43.7 | 44.3 | 39.8 | 12.3% → 31.6% |
| DR_REQREP latency ×C | 0.07 | 1.62 | 0.89 | 18.8 | 9.2 | 5.1 | |
| RR_REQREP %C | 19.1 | 18.6 | 19.0 | 36.0 | 37.0 | 35.3 | 11.4% → 27.5% |
| RR_REQREP latency ×C | 4.2 | 2.6 | 0.13 | 22.8 | 11.1 | 5.8 | |

- 작은 크기의 latency 배율이 0.07~4.2로 흩어지는 것은 C 셀의 양안정(깊이가 run마다 수백~수천으로
  바뀜) 때문이고, 큰 크기 5~23x는 창 16/8/4 vs C 실제 깊이 1.9의 창 모델 한계(C++·.NET·Go와 같다).
- 작은 크기 19~22%는 창이 아니라 Node 요청 경로의 per-message 비용이다
  (`2026-09-08-node-cost-map.ko.md`: recv materialization·Promise 정산). 러너 정합으로는 더 못 올린다.
- §8의 Go latency `/2.0`은 `2026-09-08-single-reqrep-parity-rust-go.ko.md`에서 Rust와 함께 고쳤다(`98a8717872`).
