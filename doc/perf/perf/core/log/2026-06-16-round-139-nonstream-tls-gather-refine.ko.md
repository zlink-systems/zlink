# Round 139: non-STREAM TLS gather refine

## 목표

- round138에서 확인한 `STREAM/tls` 하락 가능성을 줄이기 위해 encrypted tiny gather를 non-STREAM 경로로 좁힌다.
- `PUBSUB/tls` 개선 후보는 유지하되 STREAM 전용 fast path와 분리한다.

## 변경

- `asio_engine_t::prepare_gather_output()`의 `tiny_encrypted_gather` 조건에 `!stream_mode`를 추가한다.
- SSL transport `async_writev()` 지원은 유지한다.

## 판단 기준

- 직접 관련 CTest가 통과한다.
- `PUBSUB` all-transport와 `STREAM` all-transport focused perf에서 round138 하락이 완화된다.
- 하락 항목이 반복되면 TLS gather 후보 전체를 되돌린다.

## 빌드와 테스트

- `cmake --build core/build --target libzlink -j$(nproc)`: pass
- 첫 `ctest --test-dir core/build --output-on-failure -R 'tls|asio|pubsub|xpub|xsub'`:
  13/14 pass, `test_spot_pubsub_scenario_node_child_interop` 1회 fail.
- 분리 재실행:

```bash
ctest --test-dir core/build --output-on-failure \
  -R '^test_spot_pubsub_scenario_node_child_interop$' --repeat until-fail:5
```

결과: 5/5 pass.

- 직접 관련 테스트 재실행:

```bash
ctest --test-dir core/build --output-on-failure -R 'tls|asio|pubsub|xpub|xsub'
```

결과: 14/14 pass.

## focused perf 명령

```bash
sleep 45; uptime; PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern PUBSUB,STREAM --transports tcp,tls,ws,wss \
  --duration 5 --runs 5 --connect-ready-timeout-ms 5000 \
  --results-tag round139_nonstream_tls_gather_pubsub_stream_focus
```

## focused perf 결과

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_033428_round139_nonstream_tls_gather_pubsub_stream_focus.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `0.68 2.15 2.57`
- 완료: success 8, fail 0

64B throughput:

| pattern/transport | current | vs round138 | vs round134 |
|-------------------|---------|-------------|-------------|
| PUBSUB/tcp | 2696133.0 | +3.66% | +4.71% |
| PUBSUB/tls | 2375688.6 | -2.05% | -0.28% |
| PUBSUB/ws | 2288647.0 | +0.38% | +4.73% |
| PUBSUB/wss | 2677326.4 | -0.09% | +0.26% |
| STREAM/tcp | 298517.8 | -6.37% | -6.27% |
| STREAM/tls | 224968.0 | +3.71% | -0.19% |
| STREAM/ws | 273103.6 | -0.27% | +0.13% |
| STREAM/wss | 191839.4 | +0.29% | -1.73% |

## 판단

- non-STREAM 축소로 `STREAM/tls`는 round138 대비 회복됐다.
- 그러나 `PUBSUB/tls`는 round134 대비 -0.28%로 사실상 개선이 사라졌다.
- heuristic과 SSL gather 인터페이스를 유지할 만큼의 안정적인 이득이 없다.
- POSD 기준으로 복잡도 대비 효과가 부족하므로 TLS gather 후보 전체를 되돌린다.
- 현재 유지 후보는 `SPOT_SENDSEND` 단일 FINAL fast path 하나로 되돌린다.

## 되돌림

- TLS gather 관련 변경 제거:
  - `asio_stream_fastpath_policy::encrypted_tiny_gather_threshold()`
  - `asio_engine_t::prepare_gather_output()`의 encrypted tiny gather 분기
  - `ssl_transport_t::supports_gather_write()`
  - `ssl_transport_t::async_writev()`
- `cmake --build core/build --target libzlink -j$(nproc)`: pass
- `git diff --check`: pass
- `git diff -- core/src core/include core/tests` 기준 남은 source diff:
  - `core/src/api/spot/request_reply/service_spot_request_reply_part_submit.cpp`
  - `zlink_spot_send_spot_part()` 단일 `ZLINK_PART_FINAL` fast path만 유지
