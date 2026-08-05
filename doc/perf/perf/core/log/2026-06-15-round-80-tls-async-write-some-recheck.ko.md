# Round 80 - TLS async_write_some recheck

## 목표

- 이전에 효과가 작아 되돌렸던 TLS write 후보를 May26 기준과 현재 기준으로 다시 검토한다.
- 새 상태나 캐시를 추가하지 않고 transport 내부 구현만 조정한다.
- 하락 항목이 있거나 focused 효과가 노이즈 수준이면 변경을 되돌린다.

## 후보

- `ssl_transport_t::async_write_some()` 내부에서 `boost::asio::async_write()` 대신 SSL stream의 `async_write_some()`을 호출한다.
- 상위 `asio_engine_t::on_write_complete()`는 부분 write를 처리하고 남은 바이트를 이어서 async write로 보낸다.
- transport 메서드 이름과 실제 부분 write 동작이 맞아져서 구현 의미가 단순해진다.

## POSD 검토

- 인터페이스는 바꾸지 않는다.
- 새 상태, 특수 캐시, workload 전용 분기를 추가하지 않는다.
- 복잡도는 상위 엔진의 기존 부분 write 처리 경로에 맡기고 transport는 한 번의 write_some을 시작하는 책임만 가진다.

## 검증 계획

- `cmake --build core/build -j$(nproc)`
- focused PUBSUB/tls/64B perf.
- focused 결과가 유의미하면 다른 TLS 항목도 확인한다.

## 검증 결과

- build: `cmake --build core/build -j$(nproc)` 통과.
- tests: `ctest --test-dir core/build --output-on-failure -R 'test_(transport_matrix|pubsub|pubsub_filter_xpub|xpub_nodrop|stream_socket|stream_threadsafe|stream_fastpath|multi_socket_contract_regressions)$'`
  - 결과: 8/8 통과.
- perf:
  - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern PUBSUB --transports tls --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round80_tls_async_write_some_pubsub_tls`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_135319_round80_tls_async_write_some_pubsub_tls.txt`
  - load_avg: `6.68 14.24 12.72`
  - result: `MULTI_PUBSUB/tls/64B = 2,230,807.0 ops/s`

## 판정

- round78 current `2,293,853.4 ops/s` 대비 약 `-2.75%`다.
- 이전 측정에서 작게 좋아 보였던 신호는 재현되지 않았다.
- 하락 후보이므로 변경을 되돌렸다.
