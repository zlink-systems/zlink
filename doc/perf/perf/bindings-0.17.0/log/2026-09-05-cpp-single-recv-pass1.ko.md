# C++ Single 수신 경로 자체 hot-path pass 1 — library no-go, 러너 버그 수정, 재짝지음

[before](2026-09-05-cpp-single-before.ko.md)에서 "C++ 수신 경로가 병목이라 큐가 HWM까지 찬다"로 해석한 single one-way 셀의 자체 개선 pass
(계획서 §7.4-9~11). codex sol high job(브리프 `doc/plan/c016-worklog/briefs/cpp-perf-single-recv-pass1.b.prompt`, 요약
`doc/plan/c016-worklog/cpp-perf-single-recv-pass1-summary.md`, 07:17~07:35 KST, worktree detached @ `b2995799ac`).

## 비용 위치 (callgrind PAIR/tcp/64B, part-count 2, 1초, `--separate-threads`)

| 항목 | C++ before | C | 판정 |
|---|---|---|---|
| 수신 스레드 전체 | 7.99 kIr/msg | 6.25 kIr/msg(poller 제외) | |
| runner `measurement_payload_part` | 1.72 kIr/msg, **`getenv` 2회/msg** | `getenv` 1회/msg | **가장 큰 C++ 전용 비용, 러너 버그** |
| library `socket_t::receive` | 5.58 kIr/msg | C helper 5.38 kIr/msg(runner getenv 제외) | 잔여 3.7% — 5% library 후보 아님 |
| `received_t`/parts | wrapper 1개 재사용, 메시지당 heap 0 | raw part 2개 | vector capacity 재사용 이미 적용 |
| wrapper move/destruct | 6회/msg | 없음 | 2~3%, 공개 vector 계약 유지 시 제거 불가 |
| native init/close | 2/2회/msg | 2/2회/msg | 동일 |
| EAGAIN | 정수 반환, throw 0회 | 정수 반환 | 공개 `recv(received_t&, flags)`는 이미 no-throw 경로 |
| 수신 payload copy | 0 | 0 | Core가 native storage로 직접 씀 |
| 송신 스레드 | 6.57 kIr/msg, active 수신 5,836/제출 11,504 | 5.24 kIr/msg, 수신 10,990/제출 11,176 | C++도 송신이 수신을 앞서 큐를 채움 → 병목은 수신 |

수정 뒤 C++ 수신 스레드 7.99→6.43 kIr/msg(−19.6%), 송신 6.57→5.70; `socket_t::receive`는 5.58 그대로 → 개선분은 러너에만 있음.

## 변경

- library: **없음(no-go)** — 공개 API·ownership·빈 frame 안전성을 유지하며 5% 이상 줄일 후보 없음.
- 러너 버그 수정(library 효과와 합산하지 않음, 가이드 §5): `bindings/cpp/perf/single/common/perf_single_common.hpp`의
  `measurement_part_count()`가 메시지마다 `PERF_PART_COUNT`를 `getenv`/`strcmp`로 다시 읽던 것을 프로세스당 1회로. 커밋 `9cb8a3a11b`.
  측정 의미·scheduler·drain·fairness 불변. gate: contract 16/16, samples 7/7, optimization_guard 5/5, `git diff --check`.

## job after (C++만, 러너 수정 뒤, 1 run; C 기준은 06:08~06:30 paired before) — 참고값

| pattern | transport | before/C | after/C | latency before→after |
|---|---|---|---|---|
| PAIR | tcp | 87.7% | 97.9% | 19.2x→11.0x |
| PAIR | ws | 91.9% | 98.3% | 1196x→872x |
| PAIR | inproc | 82.0% | 92.2% | 598x→429x |
| PUBSUB | tcp | 103.3% | 104.8% | 4.0x→4.9x |
| PUBSUB | ws | 100.7% | 95.9% | 489x→423x |
| PUBSUB | inproc | 87.0% | 103.4% | 6.6x→2.0x |
| DEALER_DEALER | tcp | 88.1% | 94.9% | 454x→81x |
| DEALER_DEALER | ws | 89.8% | 95.3% | 659x→504x |
| DEALER_DEALER | inproc | 57.0% | 65.1% | 195x→159x |

after report `perf_cpp_single_linux_20260905_072455.txt`(worktree, 54/54 complete). 판정용 값은 아래 재짝지음 표.

## 재짝지음 (판정용, 러너 수정 뒤 C 직후 C++, `p1cpp-single-fix`, 07:37 KST~)

| pattern | transport | aggregate throughput | latency 평균 | 목표 | 상태 |
|---|---|---|---|---|---|
| `PAIR` | `tcp` | 90.0% | 13.40x(제외) | 95% | `미달(90.0%)` |
| `PAIR` | `ws` | 94.1% | 984.25x(제외) | 95% | `미달(94.1%)` |
| `PAIR` | `wss` | 93.9% | 942.96x(제외) | 95% | `미달(93.9%)` |
| `PAIR` | `tls` | 95.4% | 26.51x(제외) | 95% | `통과(95.4%)` |
| `PAIR` | `inproc` | 86.3% | 466.30x(제외) | 95% | `미달(86.3%)` |
| `PAIR` | `ipc` | 95.9% | 9.82x(제외) | 95% | `통과(95.9%)` |
| `PUBSUB` | `tcp` | 112.9% | 4.60x(제외) | 95% | `통과(112.9%)` |
| `PUBSUB` | `ws` | 101.0% | 431.50x(제외) | 95% | `통과(101.0%)` |
| `PUBSUB` | `wss` | 94.8% | 482.39x(제외) | 95% | `미달(94.8%)` |
| `PUBSUB` | `tls` | 96.1% | 15.20x(제외) | 95% | `통과(96.1%)` |
| `PUBSUB` | `inproc` | 91.1% | 2.17x(제외) | 95% | `미달(91.1%)` |
| `PUBSUB` | `ipc` | 113.2% | 4.24x(제외) | 95% | `통과(113.2%)` |
| `DEALER_DEALER` | `tcp` | 92.6% | 72.95x(제외) | 95% / 완화 90% | `통과(92.6%)` |
| `DEALER_DEALER` | `ws` | 95.8% | 554.06x(제외) | 95% / 완화 90% | `통과(95.8%)` |
| `DEALER_DEALER` | `wss` | 94.4% | 507.93x(제외) | 95% / 완화 90% | `통과(94.4%)` |
| `DEALER_DEALER` | `tls` | 96.3% | 290.99x(제외) | 95% / 완화 90% | `통과(96.3%)` |
| `DEALER_DEALER` | `inproc` | 63.1% | 154.78x(제외) | 95% / 완화 90% | `미달(63.1%)` |
| `DEALER_DEALER` | `ipc` | 94.0% | 7.95x(제외) | 95% / 완화 90% | `통과(94.0%)` |
| `DEALER_ROUTER` | `tcp` | 95.2% | 6.99x(제외) | 85% | `통과(95.2%)` |
| `DEALER_ROUTER` | `ws` | 95.6% | 529.42x(제외) | 85% | `통과(95.6%)` |
| `DEALER_ROUTER` | `wss` | 96.1% | 533.81x(제외) | 85% | `통과(96.1%)` |
| `DEALER_ROUTER` | `tls` | 97.3% | 296.04x(제외) | 85% | `통과(97.3%)` |
| `DEALER_ROUTER` | `inproc` | 90.6% | 160.00x(제외) | 85% | `통과(90.6%)` |
| `DEALER_ROUTER` | `ipc` | 95.0% | 5.92x(제외) | 85% | `통과(95.0%)` |
| `ROUTER_ROUTER` | `tcp` | 94.9% | 54.25x(제외) | 85% | `통과(94.9%)` |
| `ROUTER_ROUTER` | `ws` | 95.9% | 536.27x(제외) | 85% | `통과(95.9%)` |
| `ROUTER_ROUTER` | `wss` | 98.2% | 457.66x(제외) | 85% | `통과(98.2%)` |
| `ROUTER_ROUTER` | `tls` | 97.1% | 223.37x(제외) | 85% | `통과(97.1%)` |
| `ROUTER_ROUTER` | `inproc` | 73.8% | 146.85x(제외) | 85% | `미달(73.8%)` |
| `ROUTER_ROUTER` | `ipc` | 95.8% | 7.04x(제외) | 85% | `통과(95.8%)` |

C `perf_c_single_linux_20260905_07{3736,4600,5521}_p1cpp-single-fix.txt`·`_08{0331,1141}_…`, C++ 같은 tag(각 행의 report 파일명은 계획서 §9.1.1). 5 pattern × 6 transport 60 report 모두 complete.
판정 규칙(D-B91): one-way·routed one-way의 평균 latency는 두 러너 모두 큐 깊이(C 송신 병목=빈 큐, C++ 수신 병목=HWM 큐)라 판정에서 제외하고 처리량 aggregate로 판정한다. DD는 tcp의 완화 목표 90%를 모든 transport에 적용.
남은 미달: `inproc` DD 63.1%·RR 73.8%·PAIR 86.3%·PUBSUB 91.1%, `tcp` PAIR 90.0%·DD 92.6%, `ws/wss` PAIR 94.1/93.9%·PUBSUB wss 94.8%·DD wss 94.4%. `inproc`은 전송 비용이 없어 수신 wrapper 고정 비용(약 3.7%+move/destruct 2~3%) 비중이 가장 큰 transport이며 library 후보는 pass 1에서 소진 → Sol pass 2(read-only)로 넘긴다.
