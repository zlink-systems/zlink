# Round 140: STREAM TCP hot path rebaseline

## 목표

- TLS gather 후보를 되돌린 뒤, 현재 유지 상태에서 `STREAM/tcp 64B` 기준을 다시 잡는다.
- 기존에 기각된 gather/inflight/current-pipe 후보를 반복하지 않고, 실제 call path 근거로 다음 후보를 좁힌다.

## 기준

- May26 full:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- round134 current retained SPOT reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_014400_round134_current_retained_spot_reduced_full.txt`
- round138 rejected TLS gather reduced full:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_030017_round138_retained_spot_tls_gather_reduced_full.txt`
- round139 rejected non-STREAM TLS gather focused:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_033428_round139_nonstream_tls_gather_pubsub_stream_focus.txt`

## 시작 상태

- 유지 source diff:
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path
- TLS gather 후보는 round139에서 되돌렸다.
- perf runner/client/server는 수정하지 않는다.
- 보안 하드닝 항목은 수정하지 않는다.

## 병목 가설

1. `STREAM/tcp 64B`는 stream socket dispatch 자체보다 ASIO TCP write/speculative write와 perf echo send serialization의
   상호작용에 민감하다.
2. `stream_t::xsend()`의 routing-id 해석, direct pipe write, write credit refresh가 64B echo에서 반복 비용이 될 수 있다.
3. 기존에 기각된 `_dispatch_inflight` 제거, current pipe resolve 제거, tiny gather 계열은 이번 라운드에서 반복하지 않는다.

## 먼저 검증할 가설

- SPOT-only retained 상태의 `STREAM tcp,tls,ws,wss 64B` current를 낮은 부하에서 다시 측정한다.
- current 기준이 round134/round138보다 낮으면 source 변경보다 run-order/load 변동으로 분리한다.

## current 측정 명령

```bash
sleep 45; uptime; PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports tcp,tls,ws,wss \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round140_stream_current_after_tls_revert
```

## current 측정 결과

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_034212_round140_stream_current_after_tls_revert.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.62 1.74 2.30`
- 완료: success 4, fail 0

64B throughput:

| transport | current | vs May26 full | vs round134 | vs round138 |
|-----------|---------|---------------|-------------|-------------|
| tcp | 322010.8 | +5.52% | +1.11% | +1.00% |
| tls | 220441.4 | +2.73% | -2.20% | +1.63% |
| ws | 268237.4 | +6.74% | -1.66% | -2.05% |
| wss | 200559.2 | +8.57% | +2.74% | +4.84% |

판단:

- TLS gather 되돌림 뒤 STREAM/tcp는 round134와 비슷한 수준으로 회복했다.
- `STREAM/tcp`는 May26 full보다 높지만 400kops 목표에는 아직 도달하지 못했다.
- 다음 단계는 STREAM send/dispatch call path를 코드 기준으로 다시 좁힌다.

## STREAM call path 재확인

핵심 경로:

1. perf server packet callback은 packet frame을 만든 뒤
   `perf_zlink_send_rid_parts(..., ZLINK_DONTWAIT)`를 호출한다.
2. core API는 `send_stream_message()`에서 callback TLS의 current routing id와 caller가 넘긴 rid를
   비교한다.
3. 같은 rid이면 `stream_dispatch_send_current_msg_from_io()`가 current pipe의 peer로 바로 쓴다.
4. `stream_t::xstream_dispatch_msg()`는 steady-state에서 pipe의 server routing id를 읽고, packet parser를
   통해 header/body 메시지를 handler로 넘긴다.

재검토 결과:

- current pipe 조회 중복 제거는 round31/49/85에서 이미 같은 방향으로 검증했고 원복했다.
- `_dispatch_inflight` 완화/제거와 packet complete-frame fast path도 round132/114 계열에서 하락 또는
  효과 없음으로 원복했다.
- tiny gather와 single-write/read-drain 계열은 round62/133/138/139에서 하락 항목이 있어 유지하지 않았다.
- `send_stream_message()`의 STREAM type check 제거도 round91에서 효과가 없어 원복했다.

따라서 같은 후보를 다시 적용하지 않는다. 작은 중복 제거가 코드상 가능해 보여도, 이미 측정에서
반복 개선을 만들지 못한 항목이면 POSD 기준상 hot path에 예외 규칙을 늘리지 않는다.

## TCP stats probe

진단 명령:

```bash
cmake --build core/build --target libzlink -j$(nproc) && sleep 30 && uptime && \
ZLINK_ASIO_TCP_STATS=1 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports tcp \
  --duration 3 --runs 1 --connect-ready-timeout-ms 5000 \
  --results-tag round140_stream_tcp_stats_probe
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_034707_round140_stream_tcp_stats_probe.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.50 1.05 1.86`
- 완료: success 1, fail 0
- `STREAM/tcp 64B`: `314508.667`

판단:

- `ZLINK_ASIO_TCP_STATS=1`을 켰지만 runner/report 출력에는 `[ASIO_TCP_STATS]` 라인이 남지 않았다.
  자식 프로세스 stderr 수집 경로가 이 진단에는 충분하지 않아 I/O call counter 근거로 쓰지 않는다.
- 이 실행은 진단용이므로 개선/회귀 판정에는 쓰지 않는다.

## 현재 판단

- May26 full 기준으로는 현재 `STREAM/tcp 64B`가 `+5.52%`이고, 같은 retained 상태인 round134 대비도
  `+1.11%`다.
- 400kops 목표에는 아직 부족하지만, round62 기록상 perf helper의 `send_mutex` 제거 진단에서만
  `379K~386K ops/s`까지 회복했고, 그 변경은 perf code라 이번 core-only 범위 밖이다.
- core-only STREAM 후보 중 하락 없이 반복 개선을 보인 항목은 현재 없다.
- 따라서 STREAM/tcp에 대해서는 새 source 변경을 남기지 않고, 유지 diff는 SPOT FINAL fast path 하나로
  제한한다.
