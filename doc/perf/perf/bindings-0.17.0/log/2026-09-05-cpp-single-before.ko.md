# C++ Single suite before 측정 — Core 0.17.0 (paired C)

## 조건

- 2026-09-05 06:08~07:16 KST, pattern마다 C 직후 C++ 순차 실행(§7.3), 다른 perf·빌드 없음(시작 load 0.65~1.7).
- 7 pattern(`PAIR`, `PUBSUB`, `DEALER_DEALER`, `DEALER_ROUTER`, `DEALER_ROUTER_REQREP`, `ROUTER_ROUTER`, `ROUTER_ROUTER_REQREP`) ×
  transport `tcp,ws,wss,tls,inproc,ipc`(REQREP는 러너가 `inproc` 미지원) × size 64/256/1024/65536/131072/262144, 5초, 1 run, part-count 2,
  sndtimeo/rcvtimeo 200 ms, auto-HWM balanced, Core 0.17.0 local Release+LTO(`core/build`, `3480ee5d78`~`b03681422d` 문서 커밋만 차이, `core_dirty=0`),
  C++는 tcp pass 2 코드(`e6dd88fbc6`).
- 명령: `ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=4 PERF_TRANSPORTS=tcp,ws,wss,tls,inproc,ipc bash bindings/c/perf/run_benchmarks.sh --pattern <P> --msg-sizes 64,256,1024,65536,131072,262144 --duration 5 --runs 1 --results-tag p1cpp-single`
  → 같은 인자로 `bindings/cpp/perf/run_benchmarks.sh`(+`ZLINK_CPP_CORE_BUILD_DIR=$HOME/project/zlink/core/build`).
- report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260905_*_p1cpp-single.txt`, `bindings/cpp/perf/results/single/report/perf_cpp_single_linux_20260905_*_p1cpp-single.txt`(계획서 §9.1.1 각 행에 파일명). 14 report 모두 `status: complete`.

## 판정 요약 (before — 개선 pass 전, 판정 미확정)

비율 = C++/C throughput의 size 산술평균, latency = 평균 latency 비율의 산술평균. 목표(§2.1 C++): 단순 one-way 85%/95%, routed one-way 80%/85%, request/reply 75%/85%; latency 2.0x.

| pattern | transport | aggregate throughput | latency 평균 | 목표 | 상태 |
|---|---|---|---|---|---|
| `PAIR` | `tcp` | 87.7% | 19.18x | 85% / 95% | `미달(87.7%)` |
| `PAIR` | `ws` | 91.9% | 1195.83x | 85% / 95% | `미달(91.9%)` |
| `PAIR` | `wss` | 96.5% | 1089.54x | 85% / 95% | `미달(96.5%)` |
| `PAIR` | `tls` | 91.5% | 665.60x | 85% / 95% | `미달(91.5%)` |
| `PAIR` | `inproc` | 82.0% | 597.54x | 85% / 95% | `미달(82.0%)` |
| `PAIR` | `ipc` | 91.6% | 8.99x | 85% / 95% | `미달(91.6%)` |
| `PUBSUB` | `tcp` | 103.3% | 4.03x | 85% / 95% | `미달(103.3%)` |
| `PUBSUB` | `ws` | 100.7% | 489.34x | 85% / 95% | `미달(100.7%)` |
| `PUBSUB` | `wss` | 95.2% | 487.55x | 85% / 95% | `미달(95.2%)` |
| `PUBSUB` | `tls` | 96.0% | 14.76x | 85% / 95% | `미달(96.0%)` |
| `PUBSUB` | `inproc` | 87.0% | 6.56x | 85% / 95% | `미달(87.0%)` |
| `PUBSUB` | `ipc` | 108.7% | 2.64x | 85% / 95% | `미달(108.7%)` |
| `DEALER_DEALER` | `tcp` | 88.1% | 453.76x | 85% / 95% | `미달(88.1%)` |
| `DEALER_DEALER` | `ws` | 89.8% | 658.90x | 85% / 95% | `미달(89.8%)` |
| `DEALER_DEALER` | `wss` | 93.2% | 577.28x | 85% / 95% | `미달(93.2%)` |
| `DEALER_DEALER` | `tls` | 93.3% | 613.00x | 85% / 95% | `미달(93.3%)` |
| `DEALER_DEALER` | `inproc` | 57.0% | 194.91x | 85% / 95% | `미달(57.0%)` |
| `DEALER_DEALER` | `ipc` | 90.0% | 178.08x | 85% / 95% | `미달(90.0%)` |
| `DEALER_ROUTER` | `tcp` | 84.1% | 382.67x | 80% / 85% | `미달(84.1%)` |
| `DEALER_ROUTER` | `ws` | 92.4% | 696.03x | 80% / 85% | `미달(92.4%)` |
| `DEALER_ROUTER` | `wss` | 94.7% | 636.59x | 80% / 85% | `미달(94.7%)` |
| `DEALER_ROUTER` | `tls` | 96.4% | 657.77x | 80% / 85% | `미달(96.4%)` |
| `DEALER_ROUTER` | `inproc` | 64.4% | 188.45x | 80% / 85% | `미달(64.4%)` |
| `DEALER_ROUTER` | `ipc` | 91.2% | 151.54x | 80% / 85% | `미달(91.2%)` |
| `DEALER_ROUTER_REQREP` | `tcp` | 43.0% | 0.31x | 75% / 85% | `미달(43.0%)` |
| `DEALER_ROUTER_REQREP` | `ws` | 44.2% | 0.30x | 75% / 85% | `미달(44.2%)` |
| `DEALER_ROUTER_REQREP` | `wss` | 43.6% | 0.30x | 75% / 85% | `미달(43.6%)` |
| `DEALER_ROUTER_REQREP` | `tls` | 42.1% | 0.32x | 75% / 85% | `미달(42.1%)` |
| `DEALER_ROUTER_REQREP` | `ipc` | 40.6% | 0.34x | 75% / 85% | `미달(40.6%)` |
| `ROUTER_ROUTER` | `tcp` | 89.9% | 37.83x | 80% / 85% | `미달(89.9%)` |
| `ROUTER_ROUTER` | `ws` | 91.2% | 615.44x | 80% / 85% | `미달(91.2%)` |
| `ROUTER_ROUTER` | `wss` | 93.7% | 470.36x | 80% / 85% | `미달(93.7%)` |
| `ROUTER_ROUTER` | `tls` | 93.0% | 176.03x | 80% / 85% | `미달(93.0%)` |
| `ROUTER_ROUTER` | `inproc` | 75.1% | 472.14x | 80% / 85% | `미달(75.1%)` |
| `ROUTER_ROUTER` | `ipc` | 91.6% | 6.89x | 80% / 85% | `미달(91.6%)` |
| `ROUTER_ROUTER_REQREP` | `tcp` | 43.2% | 0.38x | 75% / 85% | `미달(43.2%)` |
| `ROUTER_ROUTER_REQREP` | `ws` | 45.3% | 0.29x | 75% / 85% | `미달(45.3%)` |
| `ROUTER_ROUTER_REQREP` | `wss` | 46.4% | 0.29x | 75% / 85% | `미달(46.4%)` |
| `ROUTER_ROUTER_REQREP` | `tls` | 42.0% | 0.34x | 75% / 85% | `미달(42.0%)` |
| `ROUTER_ROUTER_REQREP` | `ipc` | 41.3% | 0.39x | 75% / 85% | `미달(41.3%)` |

## 읽는 법

- **one-way·routed one-way의 latency 비율(수십~수백 배)은 binding의 메시지당 지연이 아니라 큐 깊이다.** 두 러너는 같은 정의(blocking send
  flags none, EAGAIN 시 재스탬프·1 ms 재시도, payload header timestamp 기반 one-way, 같은 sample cap)를 쓴다. C는 송신이 병목이라 큐가 거의
  비어 있어 latency가 전송 시간(0.002~0.03 ms)이고, C++는 수신 경로가 송신보다 느려 큐가 HWM까지 차므로 latency가 HWM 드레인 시간(4~118 ms, p99가
  mean과 근접)이다. 따라서 throughput 비율은 C++ 수신 경로의 속도를 나타내고, latency는 "수신이 병목"이라는 신호로 읽는다.
  → single one-way의 개선 대상은 수신 경로(`zlink::socket_t::recv(received_t&, flags)`, `received_t` parts vector·`message_t` RAII, poller
  wrapper, EAGAIN 예외 경로)다. 자체 pass 1 job(브리프 `doc/plan/c016-worklog/briefs/cpp-perf-single-recv-pass1.b.prompt`) 07:17 KST 시작.
- `inproc`만 크게 낮다(PAIR 82.0%, PUBSUB 87.0%, DD 57.0%, DR 64.4%, RR 75.1%): 전송 비용이 없어 binding 고정 비용 비중이 가장 큰 transport.
- REQREP 40.6~46.4%(latency 0.29~0.39x)는 multi REQREP tcp `보류` 57.4/68.4%와 같은 REQUEST async 경로 비용이며 single에서는 요청 1개씩 완료를
  기다리므로 비중이 더 크다. multi에서 두 pass를 마친 경로이지만 single 고유 비용(완료 대기·future)이 있는지 별도 pass가 필요하다(다음 작업).
