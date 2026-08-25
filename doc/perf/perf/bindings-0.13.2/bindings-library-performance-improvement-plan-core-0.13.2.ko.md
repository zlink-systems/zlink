# Core 0.13.2 bindings 라이브러리 성능 개선 계획

> 시작일: 2026-08-25
>
> 작업 브랜치: `core-0.13.2-bindings-performance` (최신 `main` 병합: `0d12a1e8f5`)
>
> 기준 Core: local worktree의 `VERSION=0.13.2`, `core/build`

이 문서는 `bindings-library-performance-improvement-plan-core-template.ko.md`를 0.13.2의
local Core 측정용으로 새로 시작한 실행 문서다. 0.13.0 문서, report와 판정은 이 문서의
기준값이나 완료 근거로 사용하지 않는다. 원시 반복값과 비교 결과는 같은 폴더의
`bindings-library-performance-core-0.13.2.xlsx` 및 `log/`에 기록한다.

## 1. 기준과 재현 조건

- C 기준과 모든 binding은 현재 branch의 local Core를 사용한다. runner에는
  `ZLINK_CORE_SOURCE=local`을 명시하고 `--core-version`을 사용하지 않는다.
- 각 paired 비교는 같은 Core revision, local runtime 경로, suite, pattern, transport,
  message size, duration, runs, client 수, auto-HWM profile, Effective Options를 사용한다.
- C를 먼저 측정한 직후 같은 조건으로 binding을 측정한다. Core 또는 binding source가
  바뀌면 해당 C 기준부터 다시 측정한다.
- report의 `META,core_source,local`, `META,core_version,0.13.2`, `META,commit`과
  `PERF_CORE_RUNTIME`이 모두 기록되어야 한다. 다른 runtime·release 결과는 참고용일 뿐이다.

## 2. 범위

| 순서 | 언어 | perf 경로 |
|---:|---|---|
| 1 | C++ | `bindings/cpp/perf` |
| 2 | .NET | `bindings/dotnet/perf` |
| 3 | Java | `bindings/java/perf` |
| 4 | Node | `bindings/node/perf` |
| 5 | Go | `bindings/go/perf` |
| 6 | Rust | `bindings/rust/perf` |
| 7 | Python | `bindings/python/perf` |

C reference는 `bindings/c/perf`다. 비율은 같은 paired 결과에서 다음처럼 계산한다.

```text
binding ratio (%) = binding throughput / C throughput * 100
```

Single 기본 size는 `64, 256, 1024, 65536, 131072, 262144` bytes다. Multi는 runner가
등록한 client 수와 size inventory를 report에 그대로 남긴다. runner에 등록되지 않은 pattern은
제외 근거를 log에 기록한다.

## 3. 목표와 판정

각 transport의 throughput gate는 size ratio의 산술평균(aggregate mean)이다. latency gate는
size별 mean latency ratio의 중앙값이다. 개별 size의 최소 기준 미달은 outlier로 기록하되,
aggregate를 통과한 transport를 그 값만으로 실패로 바꾸지 않는다.

| 언어 | 단순 one-way | routed one-way | socket request/reply | multi routed echo |
|---|---:|---:|---:|---:|
| C++ | 85% / 95% | 80% / 85% | 75% / 85% | 80% / 85% |
| .NET | 64% / 85% | 75% / 80% | 50% / 70% | 50% / 70% |
| Java | 70% / 90% | 75% / 85% | 50% / 70% | 50% / 70% |
| Node | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |
| Go | 55% / 65% | 50% / 57% | 40% / 53% | 40% / 53% |
| Rust | 85% / 95% | 70% / 85% | 70% / 85% | 70% / 85% |
| Python | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |

왼쪽은 개별 size 최소 기준, 오른쪽은 transport aggregate 목표다. C++ 단순 one-way는
개선 pass가 과도하게 길어질 때만 90% 완화 목표를 선택할 수 있으며 근거를 log에 기록한다.
C++/Rust의 latency 상한은 2.0x, .NET/Java/Go는 3.0x, Node/Python은 5.0x다.
secure transport(`tls`, `wss`)와 목표 경계 ±5%p는 C→binding 모두 5회 중앙값으로 최종
판정한다.

상태는 다음 세 가지뿐이다.

- `미측정`: 유효한 paired C/binding report가 없다.
- `통과(비율%)`: aggregate throughput·latency, 회귀, options, auto-HWM, client 수 조건을
  충족한다.
- `미달(비율%)`: aggregate throughput 또는 latency가 목표에 미치지 못했다. log에 개선 pass
  상태와 후보 결과를 반드시 기록한다.

`보류`는 성능 상태로 사용하지 않는다. 개선 pass를 모두 끝냈더라도 목표를 충족하지 못하면
상태는 계속 `미달`이다.

## 4. 강제 작업 순서

1. local Core와 대상 binding을 build하고 단일 64B smoke를 C→binding 순서로 실행한다.
2. 같은 manifest에서 모든 size를 C로 측정한 직후 binding before를 측정한다.
3. aggregate가 미달이면 **다음 transport·pattern·언어로 이동하지 않는다**.
4. profiler·allocation·copy·callback/dispatch·native boundary를 확인하고, public hot path의
   후보 A를 구현한다. 후보마다 POSDDD 책임 경계, 중복·불필요한 상태/할당/분기 제거 이득도
   별도로 평가한다. 성능 A/B의 조건을 바꾸지 않도록 리팩토링은 후보와 같은 작은 경계에
   한정하고, before/after와 기능 회귀를 기록한다.
5. 후보 A가 목표를 못 맞추거나 no-go이면 read-only Sol review를 받고, 공개 contract를
   유지하는 후보 B를 구현·측정하거나 no-go 근거를 남긴다.
6. 두 pass 후에도 목표를 못 맞추면 `미달`로 확정하고 C report, binding report, 후보 A/B,
   Sol 의견, contract 회귀 결과를 log와 시트에 남긴 뒤에만 다음 항목으로 이동한다.
7. 개선을 채택한 경우에만 source·검증·paired report·문서를 한 커밋으로 만들고 push한다.

허용되는 변경은 public interface, message/routing-id ownership, 정확히 한 번 완료, close와
failure semantics, cancellation, concurrency, callback context의 계약을 보존해야 한다. C API나
private/native handle 우회, public ownership 변경, timeout/close 동작 변경, mutex·weak reference
제거, pool 확장, large-message pool 확대는 금지한다. POSDDD 개선만으로 성능 미달을 통과로
바꾸지 않는다.

각 후보는 `doc/principal/dev/posddd.ko.md`의 POSDDD 원칙을 유지해야 한다. 처리량 개선과
별개로 정보 은닉·책임 경계·중복 제거·불필요한 상태/할당/분기 제거 이득을 평가해 기록한다.
성능 회귀, 새 복잡성, contract 위험이 있으면 그 정리는 남기지 않는다. POSDDD 이득만 있는
후보는 채택할 수 있지만 performance 상태를 `미달`에서 `통과`로 바꾸지 않으며, 성능 수치와
POSDDD 채택 근거를 같은 log·시트에 남긴다.

## 5. C++ Single 측정 표

모든 셀은 새 local Core 기준의 `미측정`으로 시작한다.

| Transport | Pattern | 64 | 256 | 1024 | 65536 | 131072 | 262144 | Aggregate / report / log |
|---|---|---|---|---|---|---|---|---|
| tcp | PAIR | 미달(94.91%) | 통과(100.44%) | 통과(97.84%) | 미달(85.87%) | 통과(95.56%) | 미달(91.62%) | **미달(94.37%)** — 후보 A 채택, 후보 B 64KiB timeout 폐기. `log/cpp-single-pair-tcp-20260825.ko.md` |
| tcp | PUBSUB | 통과(93.93%) | 통과(97.32%) | 통과(98.48%) | 통과(92.53%) | 통과(100.05%) | 통과(91.25%) | **통과(95.60%)** — latency median 1.07x. `log/cpp-single-pubsub-tcp-20260825.ko.md` |
| tcp | DEALER_DEALER | outlier(79.63%) | 통과(96.84%) | 통과(92.81%) | outlier(78.94%) | 통과(82.43%) | 통과(86.87%) | **통과(86.25%)** — latency median 1.20x; 64B·64KiB 개별 outlier는 aggregate 판정을 바꾸지 않는다. `log/cpp-single-dealer-dealer-tcp-20260825.ko.md` |
| tcp | DEALER_ROUTER | outlier(77.91%) | 통과(100.17%) | 통과(99.41%) | outlier(77.27%) | 통과(88.93%) | 통과(92.62%) | **통과(89.38%)** — latency median 1.05x; 64B·64KiB 개별 outlier는 aggregate 판정을 바꾸지 않는다. `log/cpp-single-dealer-router-tcp-20260825.ko.md` |
| tcp | DEALER_ROUTER_REQREP | 미달(66.10%) | 미달(57.12%) | 미달(38.78%) | 미달(72.29%) | 통과(80.36%) | 통과(90.54%) | **미달(67.53%)** — 후보 A 채택(66.57%→67.53%), 후보 B는 63.87% 회귀로 폐기. latency median 1.53x. `log/cpp-single-dealer-router-reqrep-tcp-20260825.ko.md` |
| tcp | ROUTER_ROUTER | 통과(82.52%) | 통과(88.62%) | 통과(94.57%) | outlier(77.59%) | 통과(85.64%) | 통과(92.88%) | **통과(86.97%)** — latency median 1.10x; 64KiB 개별 outlier는 aggregate 판정을 바꾸지 않는다. `log/cpp-single-router-router-tcp-20260825.ko.md` |
| tcp | ROUTER_ROUTER_REQREP | 미달(67.02%) | 미달(57.02%) | 미달(39.58%) | 미달(69.36%) | 통과(79.75%) | 통과(99.21%) | **미달(68.66%)** — 후보 A 66.61% 회귀, 후보 B 68.66%로 개선 없음이라 모두 폐기. latency median 1.58x. `log/cpp-single-router-router-reqrep-tcp-20260825.ko.md` |
| ws | PAIR | 미달(79.11%) | 통과(98.36%) | 통과(95.70%) | 통과(91.40%) | 통과(93.31%) | 통과(102.85%) | **미달(93.46%)** — strict 95% 목표 미달. 기존 direct single-part send·bounded pool·state reuse가 이미 적용됐고, 64KiB pool 하향은 이전 timeout 후보라 재시도하지 않음. mutex/weak 제거·pool 확대는 금지 no-go. `log/cpp-single-pair-ws-20260825.ko.md` |
| ws | PUBSUB | 통과(94.01%) | 통과(101.06%) | 통과(102.39%) | 통과(94.28%) | 통과(99.38%) | 통과(97.77%) | **통과(98.15%)** — latency median 1.06x. `log/cpp-single-pubsub-ws-20260825.ko.md` |
| ws | DEALER_DEALER | 통과(82.05%) | 통과(96.80%) | 통과(96.07%) | 통과(94.93%) | 통과(98.34%) | 통과(100.51%) | **통과(94.78%)** — latency median 1.03x. `log/cpp-single-dealer-dealer-ws-20260825.ko.md` |
| ws | DEALER_ROUTER | outlier(78.82%) | 통과(95.83%) | 통과(100.71%) | 통과(98.13%) | 통과(102.16%) | 통과(100.01%) | **통과(95.94%)** — latency median 1.07x; 64B outlier는 aggregate 판정을 바꾸지 않는다. `log/cpp-single-dealer-router-ws-20260825.ko.md` |
| ws | DEALER_ROUTER_REQREP | 미달(25.08%) | 미달(28.82%) | 미달(31.73%) | 통과(85.39%) | 통과(89.18%) | 통과(89.86%) | **미달(58.34%)** — latency median 2.19x. 후보 A는 initial target terminal contract 위반, 후보 B는 54.58% 회귀로 폐기. `log/cpp-single-dealer-router-reqrep-ws-20260825.ko.md` |
| ws | ROUTER_ROUTER | 통과(85.88%) | 통과(102.31%) | 통과(98.10%) | 통과(91.79%) | 통과(92.17%) | 통과(100.30%) | **통과(95.09%)** — latency median 1.08x. `log/cpp-single-router-router-ws-20260825.ko.md` |
| ws | ROUTER_ROUTER_REQREP | 미달(31.00%) | 미달(32.61%) | 미달(33.55%) | 통과(86.02%) | 통과(91.03%) | 통과(85.14%) | **미달(59.98%)** — 후보 B 채택(56.86%→59.98%); latency median 2.15x로 개선됐지만 request/reply 85%·2.0x 목표에는 미달. 후보 A는 exact-target contract no-go. `log/cpp-single-router-router-reqrep-ws-20260825.ko.md` |
| wss | PAIR | 통과(88.18%) | 통과(106.16%) | 통과(97.27%) | 통과(96.54%) | 통과(98.92%) | 통과(87.53%) | **통과(95.77%)** — secure 5-run median, latency median 1.07x. `log/cpp-single-pair-wss-20260825.ko.md` |
| wss | PUBSUB | 통과(95.05%) | 통과(96.73%) | 통과(96.39%) | 통과(100.53%) | 통과(101.93%) | 통과(98.90%) | **통과(98.25%)** — secure 5-run median, latency median 1.04x. `log/cpp-single-pubsub-wss-20260825.ko.md` |
| wss | DEALER_DEALER | 통과(91.20%) | 통과(97.34%) | 통과(90.86%) | 통과(103.93%) | 통과(96.12%) | 통과(83.83%) | **통과(93.89%)** — secure 5-run median, latency median 1.07x. `log/cpp-single-dealer-dealer-wss-20260825.ko.md` |
| wss | DEALER_ROUTER | 통과(95.65%) | 통과(104.07%) | 통과(98.99%) | 통과(99.39%) | 통과(107.71%) | 통과(91.14%) | **통과(99.49%)** — secure 5-run median, latency median 0.99x. `log/cpp-single-dealer-router-wss-20260825.ko.md` |
| wss | DEALER_ROUTER_REQREP | 미달(43.96%) | 미달(39.93%) | 미달(40.89%) | 통과(89.44%) | 통과(95.47%) | 통과(96.42%) | **미달(67.69%)** — secure 5-run median, latency median 1.82x. 후보 A는 exact-target contract no-go, 기존 async-only 완료 경로(B)는 유지하되 throughput 목표 85%에는 미달. `log/cpp-single-dealer-router-reqrep-wss-20260825.ko.md` |
| wss | ROUTER_ROUTER | 통과(85.73%) | 통과(97.40%) | 통과(95.26%) | 통과(97.82%) | 통과(99.70%) | 통과(92.21%) | **통과(94.69%)** — secure 5-run median, latency median 1.05x. `log/cpp-single-router-router-wss-20260825.ko.md` |
| wss | ROUTER_ROUTER_REQREP | 미달(41.85%) | 미달(38.80%) | 미달(38.61%) | 통과(89.44%) | 통과(90.03%) | 통과(87.36%) | **미달(64.35%)** — secure 5-run median, latency median 1.86x. 후보 A는 exact-target contract no-go, 기존 async-only completion 후보 B는 contract 5/5 통과 상태로 유지하되 throughput 목표 85%에는 미달. `log/cpp-single-router-router-reqrep-wss-20260825.ko.md` |
| tls | PAIR | 미달(90.41%) | 통과(101.12%) | 통과(99.16%) | 미달(92.09%) | 미달(88.67%) | 미달(81.07%) | **미달(92.09%)** — secure 5-run median, latency median 0.99x. bounded pool 후보 A는 이미 baseline에 반영됐고 64KiB pool 후보 B는 기존 timeout no-go. `log/cpp-single-pair-tls-20260825.ko.md` |
| tls | PUBSUB | 통과(88.23%) | 통과(102.62%) | 통과(94.91%) | 통과(87.35%) | 미달(81.85%) | 통과(97.83%) | **미달(92.13%)** — secure 5-run median, latency median 1.11x. single-part direct publish와 bounded pool은 이미 baseline에 반영됐고, mutex/lifetime guard 제거·pool 확대는 no-go. `log/cpp-single-pubsub-tls-20260825.ko.md` |
| tls | DEALER_DEALER | 통과(84.62%) | 통과(98.89%) | 통과(94.83%) | 통과(80.48%) | 통과(86.20%) | 통과(80.34%) | **통과(87.56%)** — secure 5-run median, latency median 1.14x. `log/cpp-single-dealer-dealer-tls-20260825.ko.md` |
| tls | DEALER_ROUTER | 통과(84.51%) | 통과(102.94%) | 통과(97.76%) | 통과(85.58%) | 통과(81.93%) | 통과(87.04%) | **통과(89.96%)** — secure 5-run median, latency median 1.10x. `log/cpp-single-dealer-router-tls-20260825.ko.md` |
| tls | DEALER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tls | ROUTER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| tls | ROUTER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | PAIR | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | PUBSUB | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | DEALER_DEALER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | DEALER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | DEALER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | ROUTER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| inproc | ROUTER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | PAIR | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | PUBSUB | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | DEALER_DEALER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | DEALER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | DEALER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | ROUTER_ROUTER | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |
| ipc | ROUTER_ROUTER_REQREP | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | |

## 6. 다음 작업

1. `PAIR / tcp`는 strict target 기준 `미달`로 확정했고, 후보 A/B와 read-only review 결과를
   기록·commit·push했다.
2. `PUBSUB / tcp`, `DEALER_DEALER / tcp`, `DEALER_ROUTER / tcp`의 C→C++ 64B smoke와 6-size
   paired final 측정을 기록했다. routed one-way의 개별 outlier는 aggregate 통과를 바꾸지 않는다.
3. `DEALER_ROUTER_REQREP / tcp`는 후보 A/B와 POSDDD gate를 마쳤지만 목표에 미달했다. 후보 A만
   유지하고 후보 B는 회귀로 폐기했다. `ROUTER_ROUTER / tcp`는 aggregate를 통과했다.
   `ROUTER_ROUTER_REQREP / tcp`도 후보 A/B와 contract 검증을 끝냈으나 68.66%로 미달했고,
   후보 A는 회귀·후보 B는 개선 없음이라 모두 폐기했다. 다음 C++ Single 항목도 같은 규칙으로
   C→C++ 순서로 측정하며, 미달이면 이 문서 4절의 개선 pass를 끝낸 뒤에만 이동한다.
4. `PAIR / ws`는 smoke와 6-size paired 기준선을 완료했다. 93.46%로 strict 95%에 미달했으나,
   허용 가능한 public hot-path 후보가 이미 적용된 최종 경로와 중복되고, 64KiB pool 하향은 기존
   timeout 근거가 있으며 mutex/weak 제거·pool 확대는 계약 금지라 no-go로 확정했다. 다음은
   `PUBSUB / ws`를 동일한 C→C++ 순서로 측정한다.
5. `PUBSUB / ws`는 smoke와 6-size paired 기준선을 통과했다. 다음은 `DEALER_DEALER / ws`를
   동일한 C→C++ 순서로 측정한다.
6. `DEALER_DEALER / ws`도 smoke와 6-size paired 기준선을 통과했다. 다음은
   `DEALER_ROUTER / ws`를 동일한 C→C++ 순서로 측정한다.
7. `DEALER_ROUTER / ws`도 smoke와 6-size paired 기준선을 통과했다. 다음은
   `DEALER_ROUTER_REQREP / ws`를 동일한 C→C++ 순서로 측정하고, 미달이면 개선 pass를 끝낸다.
8. `DEALER_ROUTER_REQREP / ws`는 후보 A/B와 contract gate를 마쳤지만 목표에 미달했다.
   후보 A는 public terminal contract 위반, 후보 B는 성능 회귀로 모두 폐기했다. 다음은
   `ROUTER_ROUTER / ws`를 동일한 C→C++ 순서로 측정한다.
9. `ROUTER_ROUTER / ws`는 smoke와 6-size paired 기준선을 통과했다. 다음은 마지막 WebSocket
   Single 패턴 `ROUTER_ROUTER_REQREP / ws`를 동일한 C→C++ 순서로 측정한다.
10. `ROUTER_ROUTER_REQREP / ws`는 후보 A/B와 contract gate를 마쳤다. 후보 A는 exact transport
    target 선택을 포기해 public terminal/failover contract를 바꾸므로 no-go, 후보 B는 async 전용
    완료 bridge로 56.86%→59.98%, latency 2.41x→2.15x를 확인해 채택했다. 최종 수치는 여전히
    request/reply 목표에 미달이므로 `미달`로 기록했으며, 다음은 `PAIR / wss`를 secure transport
    5-run median 규칙으로 측정한다.
11. `PAIR / wss`는 C→C++ 64B smoke와 6-size secure 5-run median을 통과했다. 다음은
    `PUBSUB / wss`를 같은 secure 규칙으로 측정한다.
12. `PUBSUB / wss`도 C→C++ 64B smoke와 6-size secure 5-run median을 통과했다. 다음은
    `DEALER_DEALER / wss`를 같은 secure 규칙으로 측정한다.
13. `DEALER_DEALER / wss`도 C→C++ 64B smoke와 6-size secure 5-run median을 통과했다. 다음은
    `DEALER_ROUTER / wss`를 같은 secure 규칙으로 측정한다.
14. `DEALER_ROUTER / wss`도 C→C++ 64B smoke와 6-size secure 5-run median을 통과했다. 다음은
    `DEALER_ROUTER_REQREP / wss`를 같은 secure 규칙으로 측정하고, 미달이면 개선 pass를 끝낸다.
15. `DEALER_ROUTER_REQREP / wss`는 secure 5-run baseline과 contract gate를 마쳤다. exact target을
    생략하는 후보 A는 public terminal/failover contract no-go이며, 기존 async-only completion 후보 B는
    이미 source에 반영되어 있다. throughput 67.69%로 미달을 확정하고 다음은 `ROUTER_ROUTER / wss`다.
16. `ROUTER_ROUTER / wss`는 C→C++ 64B smoke와 6-size secure 5-run median을 통과했다. 다음은
    마지막 WSS Single 패턴 `ROUTER_ROUTER_REQREP / wss`를 같은 secure 규칙으로 측정하고, 미달이면
    이 문서 4절의 개선 pass를 끝낸다.
17. `ROUTER_ROUTER_REQREP / wss`는 secure 5-run baseline과 개선 gate를 마쳤다. 후보 A는 initial
    exact target을 생략해 terminal/no-reroute contract를 바꾸므로 no-go다. 후보 B인 async-only
    completion bridge는 ownership·exactly-once·callback/blocking 분리를 보존하고 contract 5/5를
    통과하지만 throughput 64.35%로 request/reply 목표에는 미달이다. 다음은 `PAIR / tls`를 secure
    5-run median 규칙으로 측정한다.
18. `PAIR / tls`는 C→C++ 64B smoke와 6-size secure 5-run median을 완료했다. latency는 통과했지만
    strict throughput aggregate가 92.09%로 미달이다. bounded pool 후보 A는 현재 baseline에 이미
    적용돼 있고, 64KiB pool 후보 B는 timeout no-go이므로 재도입하지 않는다. 다음은 `PUBSUB / tls`를
    같은 secure 규칙으로 측정한다.
19. `PUBSUB / tls`는 C→C++ 64B smoke와 6-size secure 5-run median을 완료했다. latency는 통과했지만
    throughput aggregate가 92.13%로 95% 목표에 미달한다. single-part direct publish, operation-state
    reuse 및 bounded pool은 baseline에 이미 적용되어 있으며 mutex/lifetime guard 제거·pool 확대는
    contract/resource boundary no-go다. 다음은 `DEALER_DEALER / tls`를 같은 secure 규칙으로 측정한다.
20. `DEALER_DEALER / tls`는 C→C++ 64B smoke와 6-size secure 5-run median을 통과했다. 다음은
    `DEALER_ROUTER / tls`를 같은 secure 규칙으로 측정한다.
21. `DEALER_ROUTER / tls`는 C→C++ 64B smoke와 6-size secure 5-run median을 통과했다. 다음은
    `DEALER_ROUTER_REQREP / tls`를 같은 secure 규칙으로 측정하고, 미달이면 이 문서 4절의 개선
    pass를 끝낸다.

## 7. 완료 기준

- 대상 언어의 Single과 Multi runner inventory가 이 문서와 시트에 일치한다.
- 모든 최종 report는 local Core 0.13.2와 같은 manifest의 paired C/binding `status: complete`다.
- 모든 `미달`은 자체 개선 pass, Sol pass, candidate after 또는 no-go, contract/기능 회귀
  결과를 갖는다.
- report가 없는 셀은 `미측정`으로 남기며 완료를 주장하지 않는다.
- 개선 채택은 source, 검증, report, 문서가 같은 commit으로 push된 경우만 유효하다.
