# C++ perf multi PUBSUB runner parity 요약

## 차이 표

| 항목 | C canonical 의미 | 변경 전 C++ | 변경 후 C++ |
|---|---|---|---|
| server auto-HWM | client START 뒤 각 size에서 HWM 적용 → context 재계산 → snapshot | bind/connect 전 1회 재계산·snapshot | START 뒤 HWM 적용 → 재계산 → snapshot |
| SUB filter | 빈 문자열로 전체 topic 구독 | `bench` 구독 | 빈 문자열 구독 |
| receive shape | topic 길이가 `strlen("bench")`인지와 source routing-id가 NULL인지 검사 | topic 문자열 전체와 non-empty routing-id 검사 | topic 길이와 routing-id 존재 여부만 검사 |
| deadline/turn | poll 전 deadline, `min(100ms, remaining)`, receive 직전 deadline, stop 시 drain 종료 | 남은 전체 시간 poll, 성공 receive 뒤 deadline, stop 뒤 drain 지속 | poll/receive 전 deadline, 최대 100ms poll, stop 시 drain 종료 |

## 변경

- `bindings/cpp/perf/multi/src/perf_pubsub_server.cpp`
  - 초기 auto-HWM 재계산과 snapshot을 제거하고 START 수신 직후로 이동했다.
  - C와 같은 위치에서 `settings.hwm`을 양방향 HWM에 다시 적용한 뒤 재계산한다.
- `bindings/cpp/perf/multi/src/perf_pubsub_client.cpp`
  - 빈 SUB filter, C와 같은 topic 길이/source routing-id shape 판정, 100ms bounded poll, receive 직전 deadline, stop 즉시 drain 종료를 적용했다.
- 공유 helper와 다른 pattern(DD/REQREP/STREAM)은 변경하지 않았다.
- 러너 변경 — library 효과와 합산 금지(가이드 §5).

## 검증

- 지정 benchmark 1회:
  - 명령: `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=$PWD/core/build ZLINK_BUILD_JOBS=3 bash bindings/cpp/perf/run_benchmarks_multi.sh --pattern PUBSUB --transports tcp --msg-sizes 64,4096 --duration 3 --runs 1`
  - 결과: exit 0, status `complete`, success 2, fail 0.
  - C++ report: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260905_083618.txt`
- 최종 stop 처리 변경 후 target rebuild:
  - `cmake --build bindings/cpp/build --target cpp_comp_src_pubsub_client cpp_comp_src_pubsub_server -j3`: 통과.
- `bindings/cpp/perf/multi/tests`: 디렉터리 없음.
- `git diff --check`: 통과.
- 감독자 paired 3-run 재측정 전이므로 공식 측정은 요청된 1회만 수행했다.

### Auto-HWM detail 비교

기준: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260905_055350_p1cpp-pubsub-r3.txt`

| Size | Component | C SNDHWM | C RCVHWM | C++ SNDHWM | C++ RCVHWM | 판정 |
|---:|---|---:|---:|---:|---:|---|
| 64 | server/pub | 1048576 | 1048576 | 4096000 | 4096000 | 불일치 |
| 4096 | server/pub | 4096000 | 4096000 | 1048576 | 1048576 | 불일치 |
| 64 | client/sub | 2097152 | 2097152 | 2097152 | 2097152 | 일치 |
| 4096 | client/sub | 2097152 | 2097152 | 2097152 | 2097152 | 일치 |

## BLOCKERS

- server Auto-HWM exact-value 완료 조건은 충족하지 못했다. C canonical binary는 `PERF_MSG_SIZES`의 여러 size를 한 context/socket lifecycle에서 순차 처리하며 각 size마다 재계산한다. 반면 C++ `run_comparison.py`는 각 size를 독립 server/client 프로세스로 실행한다(`run_sizes_test`의 isolated lifecycle invariant). START 후 재계산 시점은 맞췄지만 재계산 이력이 달라 위 표처럼 값이 반대로 나왔다.
- lifecycle parity를 맞추려면 허용 범위 밖인 `bindings/cpp/perf/run_comparison.py` 변경이 필요하다. 특정 기준 report에 맞춘 size별 HWM 하드코딩은 auto-HWM 계약을 훼손하므로 적용하지 않았다.
- 기존 untracked `core/build`, `core/build-dev` symlink는 건드리지 않았다.
