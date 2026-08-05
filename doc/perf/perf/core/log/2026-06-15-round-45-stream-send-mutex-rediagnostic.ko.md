# Round 45: STREAM send_mutex 재진단

- 목표: `MULTI_STREAM/tcp/64B` historical baseline `400,124.6 ops/s` gap의 주원인이
  core runtime인지 benchmark-side 직렬화인지 다시 확인한다.
- 주의:
  - 이 라운드는 진단 전용이다.
  - perf helper 변경은 유지하지 않는다.
  - active core-only 목표의 성능 개선으로 계산하지 않는다.

## 진단 변경

- 파일:
  `bindings/c/perf/multi/common/perf_multi_stream_session.hpp`
- 임시 변경:
  - `session_t::send_mutex` 제거
  - immediate send와 pending drain의 `lock_guard` 제거
- 이유:
  - baseline commit `cb605c6c1`의 STREAM perf server는 echo send에서 이 mutex를 잡지 않았다.
  - 현재 core는 stream send thread-safe 계약 테스트를 가진다.

## 실행

- 명령:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round45_stream_tcp_no_send_mutex_rediagnostic`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_033524_round45_stream_tcp_no_send_mutex_rediagnostic.txt`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- load_avg: `13.27 13.88 8.22`
- 결과:
  - `MULTI_STREAM/tcp/64B`: `378,471.0 ops/s`
  - completion: `success=1`, `fail=0`, `status=complete`

## 비교

- historical baseline: `400,124.6 ops/s`
- baseline commit replay: `381,021.6 ops/s`
- current clean range:
  - round43 current sweep: `320,996.6 ops/s`
  - round41 focused clean: `325,532.2 ops/s`
- no-send-mutex diagnostic:
  - previous diagnostic: `375,799.2` to `383,141.6 ops/s`
  - this round: `378,471.0 ops/s`

## 판정

- 현재 checkout에서도 `send_mutex` 제거만으로 `378k ops/s`까지 회복한다.
- 이 값은 baseline commit replay `381k ops/s`와 거의 같다.
- 따라서 `STREAM/tcp/64B` 400k gap의 대부분은 core stream dispatch 구현 차이가 아니라
  benchmark-side 직렬화 차이로 설명된다.
- 진단 변경은 원복했다.

## 원복 확인

- `git diff -- bindings/c/perf/multi/common/perf_multi_stream_session.hpp --stat`: 출력 없음.
- `git diff -- core/src core/include core/tests bindings/c/perf --stat`: 출력 없음.
- 원복 후 `cmake --build core/build -j$(nproc)` 통과.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 유지 source 변경 없음.
- 보안 하드닝 의미 변경 없음.
