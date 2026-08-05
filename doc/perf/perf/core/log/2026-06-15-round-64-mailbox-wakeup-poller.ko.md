# Round 64: mailbox/wakeup/poller hot path

- goal: 64B one-way 공통 회귀의 mailbox/wakeup 또는 poller batch 원인을 줄인다.
- 완료 기준:
  - `MULTI_DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_SPOT` targeted 64B set에서 problem 대비
    중앙값 `+10%` 이상 또는 round51 대비 반복 `+10%` 이상 개선.
  - `cmake --build core/build -j$(nproc)` 통과.
  - 관련 core test 통과.
  - perf runner runtime이 `core/build` 아래임을 확인.
- 시작 시각: 2026-06-15 KST
- 기준 commit: `72d893595`
- 시작 git status:
  - core/perf source diff 없음.
  - dotnet 문서 변경은 이 작업과 무관하므로 건드리지 않는다.
- 기준 report:
  - baseline: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - problem: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 참고 current report:
  - round43 clean 64B sweep:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_025722_round43_current_64b_lowload_sweep.txt`
  - round51 one-way repeat:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_041202_round51_oneway_64b_repeat.txt`
- 대상 pattern/transport/size:
  - `MULTI_DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_SPOT`
  - `tcp,tls,ws,wss`
  - `64B`

## 가설

- 가설 1: 64B one-way에서는 payload 처리보다 pipe flush 뒤 command/mailbox wakeup 빈도가 병목이다.
  `pipe_t::flush_unlocked()`와 command 송신 조건을 줄일 수 있으면 DEALER_DEALER/PUBSUB/SPOT 공통으로
  개선된다.
- 가설 2: poller가 이미 처리 가능한 command를 너무 작은 batch로 처리해 같은 wakeup 비용을 반복한다.
  command 처리 loop에서 batch 또는 drain 조건을 개선하면 one-way 전체에 효과가 난다.
- 가설 3: ASIO engine의 output restart 조건이 작은 메시지에서 불필요하게 async boundary를 만들고,
  poller wakeup과 결합해 throughput을 낮춘다.
- 선택한 가설: 먼저 가설 1을 검증한다. pipe flush와 command/mailbox 경로를 코드 기준으로 추적한 뒤,
  이미 pending activation이 있는 경우 중복 wakeup을 줄일 수 있는지 본다.

## stream 기준 재확인

- 사용자 정정 기준:
  - baseline 파일:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - 목표 수치:
    `RESULT,current,MULTI_STREAM,tcp,64,throughput,400124.600`
  - `STREAM/ws`와 `STREAM/wss`는 별도 기준이며, `400kops` 목표 판정에 섞지 않는다.
- current source 상태:
  - `git diff -- core/src core/include core/tests bindings/c/perf --stat` 결과 없음.
  - `cmake --build core/build -j$(nproc)` 통과.
  - perf runner runtime:
    `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- clean 재측정 1:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round64_stream_tcp64_current_recheck`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_072117_round64_stream_tcp64_current_recheck.txt`
  - result:
    `MULTI_STREAM/tcp/64B = 323,518.8 ops/s`
  - load_avg:
    `0.84 5.07 5.31`
- rebuild 뒤 clean 재측정 2:
  - command:
    `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round64_stream_tcp64_after_rebuild`
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_072136_round64_stream_tcp64_after_rebuild.txt`
  - result:
    `MULTI_STREAM/tcp/64B = 329,790.4 ops/s`
  - load_avg:
    `0.91 4.82 5.23`
- 판정:
  - 현재 checkout에서 `250kops`는 이 두 번의 clean run에서는 재현되지 않았다.
  - 하지만 기준 `400,124.6 ops/s` 대비 여전히 약 `-17.6%`에서 `-19.2%` 미달이다.
  - round45/48/60/62의 반복 근거상 가장 큰 gap은
    `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`의
    `session_t::send_mutex` 직렬화지만, 이 작업에서는 perf helper 성능 변경을 유지하지 않는다.

## 읽은 코드와 중복 wakeup 판정

- `core/src/runtime/core/pipe.cpp`
  - `pipe_t::flush_unlocked()`는 `_out_pipe->flush()`가 reader sleep을 반환할 때만
    `send_activate_read(_peer)`를 보낸다.
- `core/src/runtime/core/ypipe.hpp`
  - `ypipe_t::flush()`는 consumer cursor가 비어 있는 경우에만 `false`를 반환한다.
    이미 깨어 있는 reader에 대해 반복 wakeup을 보내는 단순 중복은 여기서 걸러진다.
- `core/src/runtime/core/mailbox.cpp`
  - `mailbox_t::send()`는 command pipe flush가 sleeping을 반환할 때만 signaler와
    ASIO post를 사용한다.
  - `_scheduled` atomic으로 mailbox handler post는 병합된다.
- `core/src/runtime/core/io_thread.cpp`
  - `io_thread_t::process_mailbox()`는 `recv(..., 0)`가 실패할 때까지 command를 drain한다.
    작은 고정 batch 때문에 wakeup을 반복하는 구조는 보이지 않는다.
- `core/src/runtime/core/signaler.cpp`
  - eventfd count가 1보다 크면 남은 count를 다시 써서 wakeup 의미를 보존한다.
- `core/src/runtime/core/object.cpp`
  - `send_activate_write()`는 같은 thread면 직접 처리하지만, `send_activate_read()`는 command를 보낸다.
  - 같은 I/O thread `activate_read` 직접 처리 후보는 round26에서 이미 검증했고
    `STREAM/tcp/64B = 323,431.2 ops/s`로 악화되어 원복했다.
- 판정:
  - mailbox/wakeup의 단순 중복 제거 후보는 현재 코드에서 이미 대부분 병합되어 있다.
  - 같은 thread 직접 activation은 이미 성능상 실패했다.
  - 이 가설로 source 변경을 유지할 근거가 없다.

## STREAM pipe LWM 진단

- 배경:
  - `core/src/runtime/core/session_base.cpp`의 `stream_pipe_lwm_hint` 기본값은 `4`다.
  - STREAM echo에서 peer read progress command가 너무 잦으면 작은 메시지 처리량을 낮출 수 있다.
- 주의:
  - 처음 `16/32/128` 값을 병렬 실행한 결과는 벤치마크 간섭이 있으므로 판정에서 제외한다.
  - 이 병렬 실행에서 `32/128`이 238K대까지 떨어졌지만, 서로 같은 시각에 실행되어 무효다.
- 순차 진단:
  - `ZLINK_STREAM_PIPE_LWM_HINT=64`
    - report:
      `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_072332_round64_stream_tcp64_lwm64_seq_confirm.txt`
    - result:
      `MULTI_STREAM/tcp/64B = 337,514.4 ops/s`
  - `ZLINK_STREAM_PIPE_LWM_HINT=128`
    - report:
      `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_072338_round64_stream_tcp64_lwm128_seq_confirm.txt`
    - result:
      `MULTI_STREAM/tcp/64B = 336,069.8 ops/s`
  - 기본값 재비교:
    - report:
      `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_072351_round64_stream_tcp64_default_seq_compare.txt`
    - result:
      `MULTI_STREAM/tcp/64B = 337,509.8 ops/s`
- 판정:
  - LWM hint 64/128은 같은 시간대 기본값과 동률이다.
  - `stream_pipe_lwm_hint` 기본값 변경은 유지할 근거가 없다.

## STREAM gap 재판정

- round45/48/60/62의 반복 근거:
  - perf helper `session_t::send_mutex` 제거 진단은 `378K~386K ops/s`까지 회복했다.
  - round48 all-size smoke도 `success=6, fail=0`이었다.
  - baseline commit replay는 같은 머신에서 `370K~381K` 범위였다.
- 이번 round 재측정:
  - current clean은 부하와 순서에 따라 `323K~337K` 범위다.
  - 사용자가 말한 `250K`는 이번 round의 clean 단독 실행에서는 재현되지 않았다.
  - 단, 병렬 벤치마크처럼 잘못 겹쳐 돌리면 238K대가 나오므로, 낮은 stream 수치는 실행 간섭을 먼저 의심해야 한다.
- 결론:
  - corrected target은 여전히 `STREAM/tcp/64B = 400,124.6 ops/s`가 맞다.
  - 현재 core-only 후보들은 400K 복구에 실패했다.
  - 가장 큰 설명력은 benchmark-side `send_mutex` 직렬화지만, 이 작업의 성능 변경으로 유지하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 아직 없음.
- 보안 의미를 유지한 근거: 현재는 분석 단계이며 source 변경 없음.
- 추가로 실행한 회귀 테스트: source 후보가 생기면 기록한다.
