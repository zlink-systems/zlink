# Round 31: STREAM dispatch send current pipe lookup

- 목표: perf client/server 변경 없이 `MULTI_STREAM/tcp 64B` core dispatch echo hot path의 중복 작업을 줄일 수 있는지 확인한다.
- 완료 기준: clean source 대비 `STREAM/tcp 64B` +5% 이상이면 인접 stream tests/perf로 확장 검증, 5% 미만이면 원복. 전체 목표 완료 기준은 별도이며 이 라운드 단독으로 goal 완료 처리하지 않는다.
- 기준 baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt` (`MULTI_STREAM/tcp/64 = 400,124.6`)
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt` (`MULTI_STREAM/tcp/64 = 299,395.0`)
- 현재 clean recheck: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_004607_round30_stream_tcp_clean_recheck.txt` (`323,970.6`)
- 시작 git status: core source diff 없음. 기존 .NET doc 변경 및 untracked perf logs는 unrelated로 둔다.

## 병목 가설

1. STREAM packet callback echo는 `zlink_send_part_rid()` -> `send_stream_message()` -> `stream_dispatch_send_current_msg_from_io()`로 들어가며, 같은 routing id callback 내부에서는 route map lookup 없이 direct pipe write를 사용한다.
2. `stream_dispatch_send_current_msg_from_io()`가 current dispatch pipe를 한 번 읽은 뒤 `resolve_current_dispatch_output_pipe()`에서 TLS current pipe를 다시 읽는다. 작은 packet echo에서는 이 중복 TLS lookup이 반복된다.
3. 다만 round27의 direct xsend/body-view 후보가 5% 미만이었으므로, 이 후보도 5% 미만이면 즉시 폐기한다.

## 먼저 검증할 가설

- `stream_dispatch_send_current_msg_from_io()`에서 이미 읽은 `dispatch_pipe`로 direct output pipe를 계산해 중복 TLS lookup을 제거한다.
- 공개 API, perf runner/client/server, HWM/credit 의미는 변경하지 않는다.

## 검증 결과

- build: `cmake --build core/build -j$(nproc)` 통과. 중간에 clock-skew 경고가 있었지만 종료 코드는 0이었다.
- focused test: `ctest --test-dir core/build --output-on-failure -R 'test_(stream_threadsafe|stream_fastpath|stream_socket|stream_send_blocking_wakeup|multi_stream_server_reassembly|transport_matrix)$'`
  - 결과: 6/6 통과.
- 최초 perf: `round31_stream_current_pipe_lookup`는 `376,819.8`이 나왔지만, 직전 no-mutex diagnostic 이후 `comp_src_stream_server`가 perf source 원복 상태로 재링크되지 않은 상태라 오염 가능성이 있어 무효 처리한다.
- 오염 제거: `cmake --build bindings/c/build --target comp_src_stream_server -j$(nproc)`로 perf stream server를 현재 source와 다시 맞췄다.
- 유효 perf command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round31_stream_current_pipe_lookup_relinked`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_005607_round31_stream_current_pipe_lookup_relinked.txt`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- result: `MULTI_STREAM/tcp/64 = 327,025.8`.
- clean recheck `323,970.6` 대비 `+0.94%`.

## 판정

- 5% 미만이므로 성능 개선으로 인정하지 않는다.
- source 변경은 원복한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음. STREAM dispatch send 내부의 current pipe lookup만 건드렸다.
- 보안 의미를 유지한 근거: WS/WSS pending message 사본, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 변경하지 않았다.
- 추가로 실행한 회귀 테스트: stream focused 6개 테스트 통과.
