# Core hotpath instruction gate 요약

## 결과

Core public C API만 사용하는 callgrind instruction-count gate를 구현했다. 4개 cell의 기준값 생성, 3회 결정성 검증, CTest gate PASS, 인위적 회귀 FAIL을 확인했다.

## 파일 목록

- `core/tests/perf/hotpath_bench.cpp`: 4개 cell, 1024B payload, 100회 warm-up, callgrind toggle 측정 구간.
- `core/tests/perf/hotpath_gate.py`: callgrind 실행·summary 파싱·±5% 판정·표 출력·감독용 reference 갱신.
- `core/tests/perf/hotpath_reference.json`: 현재 감독관 수정본 기준 instr/msg.
- `core/tests/CMakeLists.txt`: `hotpath_bench` target과 valgrind 조건부 `hotpath_gate` CTest(label `hotpath`, timeout 600).
- `/home/hep7hep7/project/zlink-work/c016/hotpath-gate-progress.md`: 단계별 진행 기록.

금지 경로인 `doc/**`, `core/doc/**`, `framework/**`, `bindings/**`, `core/src/**`는 수정하지 않았다.

## 측정값과 결정성

기본 반복 수는 일반 cell 20,000, req/rep 5,000이다. 편차는 run 1(reference) 대비다.

| cell | run 1/reference | run 2 | run 3 | 최대 절대 편차 |
|---|---:|---:|---:|---:|
| `dealer_dealer_inproc` | 3455.0744 | 3455.0403 | 3455.4705 | 0.011464% |
| `dealer_router_reqrep_inproc` | 12056.7982 | 12063.2466 | 12059.3238 | 0.053484% |
| `pair_inproc` | 2681.4456 | 2681.6211 | 2681.5426 | 0.006545% |
| `router_router_tcp` | 2972.21735 | 2972.33265 | 2972.17565 | 0.003879% |

모든 cell이 요구한 ±0.5% 안에서 재현됐다. TCP cell은 send 수집 후 비동기 전송이 receive queue에 도착하도록 1초 안정화하고, 안정화 시간은 수집에서 제외해 command-drain scheduling 편차를 제거했다.

## 검증

- Release configure: PASS, `core/build-hp` 사용.
- 전체 build: `ulimit -v 16777216; cmake --build core/build-hp -j3` PASS.
- 4개 cell 각 10 iterations native smoke: PASS.
- 최종 CTest: `ulimit -v 16777216; ctest --test-dir core/build-hp -R '^hotpath_gate$' --output-on-failure` PASS (5.39s).
- 회귀 탐지: 메시지당 임시 `std::string` 생성·순회를 넣은 `pair_inproc`가 10179.4031 instr/msg, reference 대비 3.7962로 FAIL(exit 1). 원복 및 정상 재빌드 완료.
- 정적 검사: Python compile, `clang-format --dry-run --Werror`, `git diff --check` PASS.
- callgrind 전 모든 실행에서 `ps -eo comm | grep -E '^(perf_|python3)$'` 결과가 비어 있음을 확인했다.

전체 build에서 기존 범위 밖 `core/tests/integration/test_zmp_metadata.cpp:1766`의 enum→byte 변환 경고 1건이 있었지만 build 결과에는 영향이 없었다.

## BLOCKERS

- 최종 구현·검증 blocker는 없다.
- 규칙 문서 §5.1의 “구현 작업이 기준값을 고치지 않는다”와 이번 작업 지시의 “`--update-reference`로 기준값 작성”이 충돌한다. 더 구체적인 산출물 지시에 따라 기준값을 생성했으며, 감독관이 최종 tree에서 다시 생성한다는 지시도 반영했다.
- 환경 메모는 checkout branch를 `perf/hotpath-gate`라고 했지만 실제 branch는 `perf/phase2-judge`였다. “현재 checkout branch를 main과 동일 취급” 지시에 따라 전환 없이 작업했다.
