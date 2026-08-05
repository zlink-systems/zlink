# Round 48: stream send mutex removal diagnostic

- 목표: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`의
  `MULTI_STREAM/tcp/64 = 400,124.6 ops/s` 기준으로 STREAM 측정값을 회복한다.
- 배경:
  - baseline commit `cb605c6c1`의 STREAM perf server는 echo send에 별도
    `send_mutex`를 두지 않았다.
  - 현재 checkout은 `1b60c0159 fix: serialize C multi stream sends` 이후
    `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`에서 immediate send와
    pending drain send를 모두 `send_mutex`로 직렬화한다.
  - 현재 core에는 STREAM send thread-safe 회귀 테스트가 있고, packet callback 안의
    FINAL-only `zlink_send_part_rid()`는 core direct current-pipe path를 먼저 탄다.
- 임시 진단 변경:
  - perf helper의 `session_t::send_mutex`와 두 `lock_guard`를 제거했다.
  - 이 변경은 perf client/server 변경이므로 core 성능 개선으로 유지할 수 없다.
- 진단 판정 기준:
  - stream thread-safe 및 stream focused 테스트 통과.
  - `MULTI_STREAM/tcp/64B`가 clean 재현 대비 명확히 상승하고 baseline replay
    `381k` 근처까지 회복해야 유지한다.
- 검증:
  - build: `cmake --build core/build -j$(nproc)` 통과.
  - ctest:
    `ctest --test-dir core/build -R 'test_stream_threadsafe|test_stream_socket|test_stream_fastpath|test_stream_send_blocking_wakeup|test_multi_stream_server_reassembly|test_thread_safe_contract_policy' --output-on-failure`
    20/20 통과.
  - perf 1:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_035513_round48_no_send_mutex_stream_tcp64.txt`
    - `MULTI_STREAM/tcp/64`: `386,807.4 ops/s`
    - clean 재현 `332,541.6 ops/s` 대비 `+16.3%`
    - historical baseline `400,124.6 ops/s` 대비 `-3.3%`
  - perf 2, baseline과 같은 connect concurrency 128:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_035526_round48_no_send_mutex_stream_tcp64_connect128.txt`
    - `MULTI_STREAM/tcp/64`: `379,265.2 ops/s`
    - baseline commit replay `381,021.6 ops/s` 대비 `-0.5%`
  - STREAM/tcp all-size smoke:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_035612_round48_no_send_mutex_stream_tcp_all_sizes_smoke.txt`
    - success 6, fail 0
    - `64B`: `381,225.8 ops/s`
    - `256B`: `378,121.4 ops/s`
    - `1024B`: `357,713.8 ops/s`
    - `4096B`: `327,543.4 ops/s`
    - `65536B`: `61,212.2 ops/s`
    - `131072B`: `29,995.6 ops/s`
- 판정:
  - 유지하지 않는다. 변경은 원복했다.
  - 현재 환경에서는 2026-05-13 historical baseline 원본 `400,124.6`에는 3-5% 부족하지만,
    baseline commit replay 수준까지 회복했다.
  - 400k gap의 핵심은 core runtime 후보가 아니라 perf stream helper의 추가 직렬화였다.
  - 활성 목표의 원칙상 perf helper 변경은 성능 개선으로 인정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음.
- 보안 의미를 유지한 근거: 최종 source 변경이 없으며, WS/WSS pending message copy,
  mtrie, port parsing, IPC unlink order, decoder/message/send guard, maxmsgsize 정책을
  변경하지 않았다.
- 추가로 실행한 회귀 테스트: stream/thread-safe focused ctest 20/20 통과.
