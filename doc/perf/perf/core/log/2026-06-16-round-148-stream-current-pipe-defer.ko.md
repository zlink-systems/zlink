# Round 148: STREAM current pipe 조회 지연 A/B

## 목표

- STREAM/tcp 64B callback echo hot path에서 아주 작은 core-only 후보가 하락 없이 개선되는지 확인한다.
- 후보가 `+1~2%`라도 인접 원복 대비 하락이 없으면 유지 후보로 본다.
- perf runner/client/server 코드는 수정하지 않는다.

## 후보

- 파일: `core/src/runtime/sockets/stream/stream_dispatch_send.cpp`
- 임시 변경:
  - `stream_dispatch_send_current_msg_from_io()`에서 fallback 때만 쓰는
    `stream_dispatch_context_t::current_pipe()` 조회를 direct write 실패 이후로 지연했다.
- POSD 판단:
  - 공개 API와 readiness 의미는 바꾸지 않는다.
  - 성공 hot path에서 필요 없는 상태 조회를 늦추는 변경이라, 후보 자체는 단순화 방향이다.

## 검증

빌드:

```bash
cmake --build core/build --target libzlink -j$(nproc)
```

테스트:

```bash
ctest --test-dir core/build --output-on-failure -R 'test_(stream|transport_matrix|multi_socket_contract_regressions|socket_msg_dispatch|reconnect_options)$|unittest_stream'
```

- 결과: 3/3 passed

## Candidate 측정

명령:

```bash
sleep 45 && uptime && \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports tcp \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round148_stream_current_pipe_defer
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_052322_round148_stream_current_pipe_defer.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `1.05 1.50 2.00`
- 완료: success 1, fail 0
- `MULTI_STREAM/tcp 64B`: `311472.8`

## 인접 원복 A/B

후보만 원복하고 다시 빌드한 뒤 같은 조건으로 측정했다.

명령:

```bash
cmake --build core/build --target libzlink -j$(nproc) && \
sleep 45 && uptime && \
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh --reuse-build \
  --pattern STREAM --transports tcp \
  --duration 5 --runs 7 --connect-ready-timeout-ms 5000 \
  --results-tag round148_stream_current_pipe_defer_removed_ab
```

보고서:

`bindings/c/perf/results/multi/report/perf_c_multi_linux_20260616_052439_round148_stream_current_pipe_defer_removed_ab.txt`

- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- 시작 load average: `1.85 1.72 2.04`
- 완료: success 1, fail 0
- `MULTI_STREAM/tcp 64B`: `334090.2`

## 비교

| 기준 | delta |
|------|------:|
| candidate vs May26 full | +2.06% |
| 원복 A/B vs May26 full | +9.47% |
| 원복 A/B vs round145 low-load retry | +5.62% |
| candidate vs 인접 원복 A/B | -6.77% |
| 원복 A/B vs 400kops target | -16.48% |

## 판단

- candidate는 May26 full 기준으로는 플러스지만, 인접 원복 A/B보다 `-6.77%` 낮다.
- 따라서 “하락 항목 없이 +” 조건을 만족하지 못한다.
- 후보는 원복했다.
- source diff는 다시 SPOT SENDSEND 단일 FINAL part fast path만 남긴다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 최종 source 상태에서 건드린 보안 항목:
  - 없음.
- 임시 후보는 STREAM dispatch 내부 조회 순서만 바꿨고, 최종 source에는 남기지 않았다.
