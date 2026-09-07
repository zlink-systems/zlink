# Core 0.17.2 — `ws`에서 **왕복 패턴만** payload 크기에 비례해 무너진다 (단방향은 멀쩡)

- 보고: 머신 A (bindings 0.17.0 성능 캠페인)
- 대상 Core: 고정 prefix `~/.cache/zlink/core-pinned/0.17.2`, 태그 `core/v0.17.2`, revision `dca377aa5e`, Build ID `c5dec25e86de575e8efa79c5327b1e3d0d424f3c`, `core_dirty=0`
- 분류: **Core/transport 계층 문제**(러너 결함 아님 — 아래 2번이 근거)
- 영향: 7개 언어 전부의 `ws` 왕복 패턴(SENDSEND·REQREP) 판정이 왜곡된다. 이미 C++ `ws` REQREP 2 cell을 `보류(C 기준 이상)`로 기록했다.

## 1. 증상

같은 Core·같은 호스트에서 `tcp` 대비 `ws`의 처리량 비율이다(5-run, clients 100, duration 5, 2-part).

| ws/tcp 비율 | 64 B | 256 B | 1024 B | 4096 B | 65536 B |
|---|---:|---:|---:|---:|---:|
| **단방향** `MULTI_DEALER_DEALER` — C | 1.009 | 1.004 | 1.045 | 0.970 | **1.045** |
| **단방향** `MULTI_DEALER_DEALER` — C++ | 1.018 | 1.034 | 1.048 | 0.934 | **0.936** |
| **왕복** `MULTI_DEALER_ROUTER_SENDSEND` — C | 0.927 | 0.961 | 0.781 | 0.652 | **0.362** |
| **왕복** `MULTI_DEALER_ROUTER_SENDSEND` — C++ | 0.909 | 0.930 | 0.590 | 0.612 | **0.401** |

**단방향은 전 크기에서 페널티가 없다**(0.93~1.05). `ws`가 대형 payload를 못 다루는 것이 아니다 — 65536 B 단방향은 오히려 `tcp`보다 빠르다(C 172,652 vs 165,256 msg/s).

**왕복만 크기에 비례해 무너진다.** 64 B에서 0.91~0.93이던 비율이 65536 B에서 0.36~0.40이 된다. `tcp`에서 왕복/단방향 비가 0.49인데 `ws`에서는 0.17이다.

## 2. 러너 결함이 아닌 근거

**서로 독립적으로 구현된 두 러너(C와 C++)가 같은 곡선을 그린다.** 위 표에서 C의 0.927→0.361과 C++의 0.909→0.401은 같은 형태다. 러너 쪽 원인이라면 두 구현이 이렇게 일치할 이유가 없다.

또 같은 두 러너의 **단방향** 값은 페널티가 없으므로, 러너의 `ws` 처리 자체가 느린 것도 아니다.

## 3. 부수 증상 — 기준선의 동작점까지 달라진다

`ws` `MULTI_DEALER_ROUTER_REQREP` 65536 B에서 C 러너는 **처리량 25,849 ops/s에 latency 0.086 ms**다. 같은 셀 `tcp`는 63,265 ops/s·0.808 ms, `tls`는 41,345·2.320 ms다. **처리량은 tcp의 41%로 떨어졌는데 latency는 오히려 10배 낮다** — 파이프라인을 채우지 못하고 한 건씩 왕복하는 상태다.

같은 셀에서 C++는 34,999 ops/s·14.5 ms로 **C보다 처리량이 높다**. Core가 25.8k에서 막고 있다면 C++도 못 넘으므로, 두 러너가 서로 다른 동작점에 갇힌 형태다. 이 때문에 C++가 4096 B 131%, 65536 B 135%로 보여 aggregate가 99.50%로 부풀었고, 머신 A는 그 값을 판정에 쓰지 않고 `보류(C 기준 이상)`로 기록했다.

## 4. 전례

2026-09-05에 같은 성격을 `ws`·`wss` REQREP **4096 B**에서 관측해 `보류(C 기준 이상)`로 기록하고 C 러너의 제출 턴을 고쳤다(D-B89). 그 수정으로 4096 B는 완화됐으나 **65536 B에 같은 문제가 남아 있고**, 이번 전수 대조로 그것이 러너가 아니라 transport 계층임이 드러났다.

## 5. 재현

```bash
export ZLINK_CORE_SOURCE=release
export ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.2

# 단방향 — ws와 tcp가 같다
bash bindings/c/perf/run_benchmarks_multi.sh --pattern MULTI_DEALER_DEALER \
  --transports tcp,ws --msg-sizes 65536 --duration 5 --runs 5 --reuse-build --results-tag ws-oneway

# 왕복 — ws가 tcp의 36%
bash bindings/c/perf/run_benchmarks_multi.sh --pattern MULTI_DEALER_ROUTER_SENDSEND \
  --transports tcp,ws --msg-sizes 65536 --duration 5 --runs 5 --reuse-build --results-tag ws-echo
```

원자료 report: C `perf_c_multi_linux_20260908_{...}_q3tcp.txt`·`_q9ws.txt`, C++ `perf_cpp_multi_linux_20260908_{...}_q3tcp.txt`·`_q9ws.txt` (각 `bindings/{c,cpp}/perf/results/multi/report/`, 모두 `status: complete`).

## 6. 조사 방향 제안 (머신 A가 확정한 것이 아님)

단방향은 멀쩡하고 왕복만, 그것도 **크기에 비례해** 나빠진다는 조합이 단서다. 왕복은 매 메시지가 상대의 응답을 유발하므로 **메시지당 flush·프레임 경계 처리**가 직렬화되는 경로가 후보다. 단방향은 파이프라이닝되어 그 비용이 분산된다. WebSocket 프레임 헤더 크기는 payload에 무관하므로 헤더 오버헤드로는 크기 비례 악화가 설명되지 않는다.

## 7. 요청

1. `ws`(그리고 `wss`) 왕복 경로에서 payload 크기에 비례해 커지는 비용의 위치를 특정해 달라.
2. 수정이 되면 회귀 테스트를 Core 통합 테스트에 추가해 달라 — 같은 크기에서 단방향과 왕복의 처리량 비가 transport에 따라 크게 달라지지 않는지 확인하는 형태.
3. 수정 전까지 머신 A는 `ws`·`wss`의 왕복 패턴 셀을 `보류(C 기준 이상)`로 두고 다음 릴리스에서 다시 잰다.

머신 A는 Core를 수정하지 않으며 하위 계층 결함을 러너에서 보상하지 않는다(계획서 §5).
