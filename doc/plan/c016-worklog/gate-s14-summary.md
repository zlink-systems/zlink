# S-14 게이트 요약

## 결과

- 기준: main `c0c561ae63bb5aabbbadd44a49dbb9ccacbd6717` = `origin/main`.
  기존 `doc/plan` 작업 기록 변경 때문에 `git pull --rebase`는 Git이 거부했지만, HEAD가 원격과
  같음을 확인했으므로 pull 없이 진행했다. `core`·`bindings`·`scripts`에는 사전 변경이 없었다.
- 적용: `~/project/zlink-work/s14`의 HEAD diff를 `s14.patch`로 추출하여 `git apply --3way`로
  충돌 없이 적용했다. 변경은 다음 두 테스트뿐이며 Core 소스·공개 헤더·ABI export 변경은 없다.
  - `core/tests/integration/test_dealer_router_single_lane_contract.cpp`
  - `core/tests/unittest/unittest_single_lane_accounting.cpp`
- 빌드: `JOBS=4 scripts/build-core.sh release --lib-only` 성공, 이어서 `JOBS=4 scripts/build-core.sh dev` 성공.

## 테스트

| 검증 | 결과 |
| --- | --- |
| `test_single_lane_flow_snapshot_accounting` solo `--repeat until-fail:30` | 30/30 PASS |
| `unittest_single_lane_accounting` solo `--repeat until-fail:50` | 50/50 PASS |
| `ctest -j4 -R 'stream|pipe'` | 23/23 PASS (integration 9, regression 10, unittest 4) |
| `lane|hwm|flow|snapshot|accounting|dealer|router` suite `--repeat until-fail:5` | 60개 × 5회, 300/300 PASS |

알려진 간헐 항목 `test_stream_socket_recv_multiclient_ready_regression`은 병렬 STREAM/pipe 실행에 포함되어 PASS했다.
`unittest_request_timeout_scheduler`는 이번 범위의 `-j2` 실행 대상이 아니므로 미실행이다. STREAM/pipe 묶음의
integration label 수는 현재 CTest 등록 기준 9개이며, 전체 묶음은 23/23이다.

## 공개 인터페이스·성능

- `git diff --stat -- core/include core/src/libzlink.vers`: 비어 있음. `git diff --check`: PASS.
- `scripts/gate/README.md`에 mirror 절차가 없어 fallback을 사용했다. `zlink.h`, `zlink_enum.h`,
  `zlink_errno.h`를 C/C++/Go/Rust binding mirror와 비교한 12회 `cmp`: 모두 PASS.
- hotpath는 다른 ninja가 없을 때만 valgrind로 실행했다. 측정 직전 load average: `3.32, 2.76, 2.70`.

| hotpath cell | reference | measured | ratio | 결과 |
| --- | ---: | ---: | ---: | --- |
| dealer_dealer_inproc | 3230.922 | 3231.134 | 1.0001 | PASS |
| dealer_router_reqrep_inproc | 18663.506 | 18592.204 | 0.9962 | PASS |
| pair_inproc | 2348.457 | 2287.784 | 0.9742 | PASS |
| router_router_tcp | 2972.532 | 2874.077 | 0.9669 | PASS |
| stream_tcp | 14623.471 | 14150.102 | 0.9676 | PASS |

`with_stream` 및 `perf/c` 경량 셀은 테스트 전용 변경이라 생략했다(예상 비율 약 1.00).
메인 working tree는 적용된 테스트 patch와 작업 기록만 둔 상태이며, 커밋·스펙 문서 변경은 하지 않았다.
