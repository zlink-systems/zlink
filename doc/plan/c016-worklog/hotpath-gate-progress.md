## 2026-09-04 시작

- 실제 checkout: `perf/phase2-judge` (사용자 지시의 “현재 branch를 main과 동일 취급” 적용).
- 기존 병렬 변경: `core/src/runtime/**` 9개 파일. 본 작업은 해당 경로를 읽기만 하고 수정하지 않음.
- 확인한 계약: `core/doc/spec/core/systems/10-hot-path.ko.md` §5.1, public C API 헤더와 기존 request/reply·ROUTER/ROUTER 테스트.
- build/test 격리 경로: `core/build-hp`; 메모리 제한 `ulimit -v 16777216`; 빌드 병렬도 `-j3`.
- 계약 불일치 후보: §5.1은 구현 작업이 기준값을 고치지 않는다고 명시하지만 요청은 현재 working tree 기준값 생성을 요구함. 감독용 `--update-reference`를 구현하고 생성하되 최종 BLOCKERS에 기록 예정.

## 2026-09-04 구현·최소 검증

- 추가: `core/tests/perf/hotpath_bench.cpp`, `core/tests/perf/hotpath_gate.py`.
- 변경: `core/tests/CMakeLists.txt`에 `hotpath_bench`와 valgrind 조건부 `hotpath_gate` 등록. Core 쪽 별도 label contract 검사는 발견되지 않음.
- CMake가 valgrind `/home/hep7hep7/.local/bin/valgrind`와 헤더 `/home/hep7hep7/.local/include/valgrind/callgrind.h`를 자동 탐색함.
- `core/build-hp` Release configure 및 `hotpath_bench` 빌드 PASS (`ulimit -v 16777216`, `-j3`).
- valgrind 없이 4개 cell 각 10 iterations 기능 smoke PASS.

## 2026-09-04 기준값·결정성·gate 검증

- callgrind 소량 smoke: 4개 cell 모두 양의 `summary:` 파싱 및 표 출력 PASS.
- 초기 TCP 측정에서 비동기 command-drain 시점에 따른 2.04% 편차를 발견. TCP send/recv 수집 구간 사이에 1초의 수집 제외 안정화 구간을 추가했고 재측정 편차는 0.006%로 감소.
- 최종 기준값(run 1): dealer/dealer 3455.0744, dealer/router reqrep 12056.7982, pair 2681.4456, router/router TCP 2972.21735 instr/msg.
- run 2/3의 기준 대비 cell별 최대 절대 편차: 0.011464%, 0.053484%, 0.006545%, 0.003879%. 모두 ±0.5% 이내.
- `ctest --test-dir core/build-hp -R '^hotpath_gate$' --output-on-failure` PASS (최종 원복·전체 build 후 5.39s, label `hotpath`).
- 인위적 회귀: `raw_send`에 메시지당 `std::string` 생성과 강제 순회를 임시 추가. pair 2681.4456 → 10179.4031 instr/msg, ratio 3.7962, FAIL(exit 1) 확인. 임시 코드는 원복하고 정상 target 재빌드 완료.
- `ulimit -v 16777216; cmake --build core/build-hp -j3` PASS. 기존 `test_zmp_metadata.cpp:1766` enum→byte 경고 1건은 범위 밖이라 수정하지 않음.
- 금지 경로(`doc/**`, `core/doc/**`, `framework/**`, `bindings/**`, `core/src/**`)는 본 작업에서 수정하지 않았고 git 쓰기·local-package 실행도 하지 않음.
