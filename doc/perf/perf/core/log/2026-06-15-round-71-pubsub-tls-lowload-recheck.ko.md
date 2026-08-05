# Round 71: PUBSUB/tls 64B low-load 재확인

- goal:
  - round70 reduced full에서 남은 `MULTI_PUBSUB/tls/64B` 하락이 낮은 load 단독 실행에서도
    반복되는지 확인한다.
  - source 변경 없이 현재 retained source와 `core/build` runtime만 사용한다.
- 시작 시각: 2026-06-15 12:52:23 KST
- 시작 load_avg:
  - `/proc/loadavg`: `0.37 1.65 4.16`
- 기준:
  - May26 full corrected baseline:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
  - problem report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- round70 current:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`
  - `MULTI_PUBSUB/tls/64B`: `2,264,552.0`

## 가설

- 가설 1:
  - `PUBSUB/tls` 하락은 full/reduced-full 순서와 load 영향이 크다. 낮은 load 단독 반복에서는
    May26 full에 가까워질 수 있다.
- 가설 2:
  - 낮은 load 단독 반복에서도 May26 full 대비 `-10%` 안팎이면 core hot path나 TLS transport
    경계에 반복 가능한 결손이 남아 있다.
- POSD 기준:
  - 새 상태, 캐시, 특수 분기는 `+5%` 이상이거나 하락 항목이 없는 반복 수치가 있을 때만 후보로 둔다.
  - 안전 가드 제거와 perf runner/client/server 변경은 제외한다.

## Current 재확인

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB \
  --transports tls \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round71_pubsub_tls_lowload_recheck
```

- runner runtime:
  - `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- runner meta load_avg:
  - `0.27 1.54 4.07`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_125240_round71_pubsub_tls_lowload_recheck.txt`
- completion:
  - success: 1
  - fail: 0
  - status: complete
- result:
  - `MULTI_PUBSUB/tls/64B`: `2,265,688.2`
- comparison:
  - May26 full 대비: `-13.62%` (`2,623,065.0` -> `2,265,688.2`)
  - problem report 대비: `-7.40%` (`2,446,707.8` -> `2,265,688.2`)
  - round70 대비: `+0.05%` (`2,264,552.0` -> `2,265,688.2`)

## 후보: TLS async_write_some 의미 정렬

- 후보 변경:
  - `ssl_transport_t::async_write_some()`이 `boost::asio::async_write()` composed operation을 사용하던 것을
    실제 `stream->async_write_some()` 호출로 바꿨다.
- 후보로 본 이유:
  - `i_asio_transport::async_write_some()` 계약과 이름은 partial write completion을 허용한다.
  - `asio_engine_t::on_write_complete()`는 `_outpos/_outsize`를 갱신하고 남은 bytes를 다시
    `async_write_some()`으로 보낸다.
  - 새 상태나 새 캐시를 만들지 않고, TLS write 경계의 숨은 반복만 줄이는 방향이라 POSD 관점에서
    검증할 가치는 있었다.
- build:
  - `cmake --build core/build -j$(nproc)` 통과.
- focused CTest:
  - `ctest --test-dir core/build --output-on-failure -R 'test_(pubsub|pubsub_filter_xpub|xpub_nodrop|transport_matrix|multi_socket_contract_regressions|zmp_request_reply|backpressure_oneway_matrix|backpressure_matrix)$'`
  - 6/6 passed.
- perf:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB \
  --transports tls \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round71_ssl_async_write_some_pubsub_tls_candidate
```

- runner meta load_avg:
  - `17.60 10.15 6.81`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_125559_round71_ssl_async_write_some_pubsub_tls_candidate.txt`
- completion:
  - success: 1
  - fail: 0
  - status: complete
- result:
  - `MULTI_PUBSUB/tls/64B`: `2,278,032.0`
- comparison:
  - round71 current 대비: `+0.54%`
  - May26 full 대비: `-13.15%`

## 판정

- `PUBSUB/tls` 하락은 낮은 load 단독 실행에서도 반복됐다.
- TLS `async_write_some()` 후보는 새 상태가 없고 인터페이스 의미와 더 잘 맞지만, 측정 개선이 `+0.54%`
  잡음권에 그쳤고 시작 load도 높았다.
- `+5%` 기준에도 못 미치고 May26 full 대비 gap도 거의 남아 있으므로 채택하지 않았다.
- 후보 변경은 원복했다.
- 원복 후:
  - `cmake --build core/build -j$(nproc)` 통과.
  - `git diff -- core/src/runtime/transports/tls/ssl_transport.cpp` 출력 없음.

## 다음 판단

- 지금 남은 PUBSUB/tls 결손은 단순 TLS write completion 방식만으로 설명되지 않는다.
- 이미 기각된 후보를 반복하지 않는다:
  - PUBSUB empty-subscription active pipe 상태 추가.
  - TLS speculative write enable.
  - ASIO handler allocator 전체 확대.
  - distributor final helper.
  - small LMSG pool.
- 다음 후보가 있다면 `mtrie` match, `dist_t` matching/HWM, pipe write/flush 중 기존 상태를 재사용하는
  좁은 변경이어야 한다. 새 상태를 추가하는 후보는 하락 없는 반복 수치가 나오기 전에는 제외한다.
