# Round 136: TLS tiny gather candidate

시작: 2026-06-16 02:32:45 KST

## 목표

- `PUBSUB/tls` 64B 반복 하락을 SSL transport 내부 gather write로 줄일 수 있는지 확인한다.
- 외부 API, perf runner, 보안 하드닝은 수정하지 않는다.
- 같은 실행 창 A/B에서 하락 transport가 있으면 되돌린다.

## 후보

- `ssl_transport_t`에 header/body `async_writev()`를 추가한다.
- ASIO engine에서 encrypted transport의 128B 이하 payload만 tiny gather fast path를 허용한다.
- POSD 판단: transport 내부에서 복사 회피를 흡수하고 호출자 계약은 늘리지 않는다.

## 검증

- `cmake --build core/build --target libzlink -j$(nproc)`: pass
- `ctest --test-dir core/build --output-on-failure -R 'tls|asio|pubsub|xpub|xsub'`: 14/14 pass

## 후보 측정

명령:

```bash
sleep 45; uptime; PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern PUBSUB --transports tcp,tls,ws,wss \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round136_tls_tiny_gather_pubsub_candidate
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_023337_round136_tls_tiny_gather_pubsub_candidate.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.58 1.13 1.88`
- 완료: fail 0

64B throughput:

| transport | candidate |
|-----------|-----------|
| tcp | 2619890.4 |
| tls | 2420668.6 |
| ws | 2266443.2 |
| wss | 2694774.4 |

초기 후보 측정은 `tls/ws/wss`가 round135보다 높았지만 `tcp`가 낮았다. 순서/부하 효과를 분리하기 위해
후보를 제거한 뒤 같은 창 A/B를 실행했다.

## removed A/B

후보를 제거하고 `core/build` runtime을 다시 빌드했다.

명령:

```bash
sleep 30; uptime; PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern PUBSUB --transports tcp,tls,ws,wss \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round136_tls_tiny_gather_removed_ab_pubsub
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_023918_round136_tls_tiny_gather_removed_ab_pubsub.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `1.58 1.80 2.00`
- 완료: fail 0

64B throughput:

| transport | removed | first candidate vs removed |
|-----------|---------|----------------------------|
| tcp | 2587325.8 | +1.26% |
| tls | 2372747.4 | +2.02% |
| ws | 2246003.0 | +0.91% |
| wss | 2674432.6 | +0.76% |

첫 A/B는 하락 항목이 없었지만, candidate가 먼저 실행됐고 removed의 시작 load가 더 높았다. 후보를 다시 적용해
뒤쪽 순서에서도 같은 방향인지 repeat를 실행했다.

## candidate repeat

후보를 다시 적용하고 `core/build` runtime을 다시 빌드했다.

명령:

```bash
sleep 30; uptime; PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern PUBSUB --transports tcp,tls,ws,wss \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round136_tls_tiny_gather_pubsub_candidate_repeat
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_024517_round136_tls_tiny_gather_pubsub_candidate_repeat.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `2.50 2.60 2.29`
- 완료: fail 0

64B throughput:

| transport | candidate repeat | vs removed | vs round135 current |
|-----------|------------------|------------|---------------------|
| tcp | 2745766.6 | +6.12% | +0.93% |
| tls | 2413911.0 | +1.73% | +0.64% |
| ws | 2330931.0 | +3.78% | +4.65% |
| wss | 2706470.8 | +1.20% | +1.42% |

## 판단

- same-window removed 대비 candidate repeat가 모든 transport에서 상승했다.
- round135 current 대비도 모든 transport가 상승했다.
- 상승폭은 작지만 하락 항목이 없고, TLS gather 지원은 SSL transport 내부에 갇혀 있어 호출자 계약을 늘리지
  않는다.
- POSD 관점에서는 복사 회피 결정을 transport/engine 내부로 숨기는 깊은 모듈 변경이다. 다만 `encrypted tiny
  gather` 정책은 heuristic이므로, reduced full에서 인접 패턴 하락이 나오면 되돌린다.
- 현재 상태: 후보를 유지한다.

## 최종 확인

- `git diff --check`: pass
- 후보 유지 상태에서 직접 관련 테스트 재실행:

```bash
ctest --test-dir core/build --output-on-failure -R 'tls|asio|pubsub|xpub|xsub'
```

결과: 14/14 pass.

넓은 spot/request-reply 포함 CTest:

```bash
ctest --test-dir core/build --output-on-failure \
  -R 'tls|asio|pubsub|xpub|xsub|spot|request_reply|zmp_request_reply'
```

결과: 42/43 pass. `test_discovery_resolve_spot` 1개 실패.

실패 상세:

- `test_discovery_resolve_spot_returns_enoent_after_owner_unregister`
- `core/tests/integration/test_discovery_resolve_spot.cpp:540`
- assertion: `Expected TRUE Was FALSE`

분리 재실행:

```bash
ctest --test-dir core/build --output-on-failure \
  -R '^test_discovery_resolve_spot$' --repeat until-fail:5
```

결과: 4회 pass 후 1회 동일 assertion 실패.

판단:

- 실패는 TLS gather 후보의 직접 경로가 아니라 discovery unregister 타이밍 경로다.
- 이번 후보의 직접 관련 테스트와 PUBSUB focused perf는 통과했지만, reduced full 전에는 이 실패를 별도
  잔여 리스크로 표시한다.
