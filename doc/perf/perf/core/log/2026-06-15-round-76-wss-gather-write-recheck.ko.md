# Round 76 - WSS gather-write recheck

## 목적

- May26 full 기준에서 `MULTI_PUBSUB/wss/64B`도 약한 하락이 있어, 기존에 꺼져 있던 WSS gather-write capability를 재검토한다.
- 구현은 이미 `wss_transport_t::async_writev()`에 남아 있으므로, capability만 다시 켜는 후보는 새 상태나 캐시를 만들지 않는다.

## 후보

- `wss_transport_t::supports_gather_write()`를 `true`로 변경.

## 검증

```bash
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure -R 'test_(transport_matrix|pubsub|pubsub_filter_xpub|xpub_nodrop|multi_socket_contract_regressions)$'
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round76_wss_gather_write_pubsub_wss_candidate
```

- build: 통과
- ctest: 5/5 통과
- perf report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_133044_round76_wss_gather_write_pubsub_wss_candidate.txt`
- runtime: `core/build/lib/libzlink.so.6.0.4`
- load_avg: `33.15 21.00 12.52`
- result: `MULTI_PUBSUB/wss/64B = 2,497,609.4 ops/s`

## 판정

- round74 current `2,504,005.6 ops/s` 대비 약 `-0.26%`.
- May26 full `2,760,571.0 ops/s` 대비 하락도 해소하지 못했다.
- 후보는 되돌렸다.

## 다음

- WSS gather-write는 채택하지 않는다.
- 남은 반복 하락은 PUBSUB의 fanout/check-HWM 또는 TLS/WSS 전송 계층보다 상위의 송신 압력 쪽으로 계속 좁힌다.
