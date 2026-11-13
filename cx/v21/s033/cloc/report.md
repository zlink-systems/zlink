# GitHub #4119 반복 실행 재확인 보고서

대상 worktree는 `/root/worktree/zlink-4119-recheck`이고 커밋은 `8c63ad23b4` (#4068 병합 후 main)이다. 코드는 수정하지 않았다.

## 빌드 정보

`linux-ninja-release` preset으로 `test_cpp_framework_location_runtime` target과 필요한 의존성만 빌드했다. CMake build type은 Release이고 `CMAKE_CXX_FLAGS_RELEASE`는 `-O3 -DNDEBUG`였다. 테스트 translation unit의 실제 컴파일 명령에는 `-O3 -DNDEBUG -std=c++20 -UNDEBUG`가 있었고 `-g`는 없었다. Ninja 빌드는 88단계에서 완료됐다.

C++ Framework 빌드가 요구하는 `zlink_cpp`와 Core 1.23.0 설치본은 기존 `csrc/build-baseline-tests/CMakeCache.txt`에 기록된 prefix를 사용했다. 이 package들은 이번 작업에서 다시 빌드하지 않았다. 첫 CMake configure는 package prefix 누락으로 완료되지 않았고, prefix를 지정한 재configure와 target 빌드는 성공했다.

| 항목 | 값 |
|---|---|
| 빌드 명령 | `cpp-build-slot.sh cmake --build cloc/build --target test_cpp_framework_location_runtime -j3` |
| 바이너리 | `/root/worktree/zlink-4119-recheck/cloc/build/test_cpp_framework_location_runtime` |
| 설정·빌드 로그 | `/root/jobs/fx/cx/v21/s033/cloc/raw/configure.log`, `/root/jobs/fx/cx/v21/s033/cloc/raw/build.log` |
| 빌드 결과 | 성공, `-g` 없음 |

## 반복 실행 결과

두 루프는 각각 `measure-shared.sh` 안에서 `stress-ng --cpu 16 --cpu-load 80 --metrics-brief`와 함께 실행했다. 반복별 종료 코드, 실패 테스트 이름, assertion 파일·줄은 각 `results.json`과 `iteration-*.log`에 기록했다.

| 루프 | 반복 | 실패 | 실패 테스트와 assertion 줄 | 테스트 수 | stress-ng 실행 시간 | stress-ng 결과 | 최저 MemAvailable |
|---|---:|---:|---|---:|---:|---|---:|
| 전체 바이너리 | 60/60 | 0 | 없음 | 회당 348개 통과 | 531.489초 | 종료 코드 0, 실패 worker 0 | 21.08 GiB |
| 과거 실패 케이스 필터 | 100/100 | 0 | 없음 | 회당 4개 통과 | 100.792초 | 종료 코드 0, 실패 worker 0 | 20.88 GiB |

필터 루프는 다음 네 parameterized case만 실행했다. 모든 반복에서 각 case가 통과했다.

| 테스트 케이스 | 이전 기록의 assertion 줄 | 이번 실행 |
|---|---:|---|
| `SameTokenMemberWaitsForSpotPerActor/0` | 5192, 5194 | 100/100 통과 |
| `ReviewHeartbeatDoesNotWaitOrRestartExactRead/0` | 5044 | 100/100 통과 |
| `ReviewCutoffTerminatesReadBeforeEpisodeRelease/0` | 5036 | 100/100 통과 |
| `ReviewOnlyReadErrorsRetryOnNextRenew/1` | 5044 | 100/100 통과 |

어느 루프에서도 실패가 없어 이전 main snapshot 바이너리 비교는 실행하지 않았다. 두 루프 모두 per-iteration timeout 300초에 걸린 실행은 없었다. stress-ng는 각 루프가 끝날 때 SIGTERM으로 종료했다.

## 원시 결과 경로

원시 결과와 실행 스크립트는 `/root/worktree/zlink-4119-recheck/cloc/` 아래에 보존했다.

- 전체 루프 요약: `/root/jobs/fx/cx/v21/s033/cloc/raw/whole/results.json`
- 전체 루프 상태·메모리·stress 로그: `/root/jobs/fx/cx/v21/s033/cloc/raw/whole/status.json`, `memory.json`, `stress-ng.log`
- 전체 루프 60개 iteration 로그: `/root/jobs/fx/cx/v21/s033/cloc/raw/whole/iteration-001.log`부터 `iteration-060.log`
- 필터 루프 요약: `/root/jobs/fx/cx/v21/s033/cloc/raw/filtered/results.json`
- 필터 루프 상태·메모리·stress 로그: `/root/jobs/fx/cx/v21/s033/cloc/raw/filtered/status.json`, `memory.json`, `stress-ng.log`
- 필터 루프 100개 iteration 로그: `/root/jobs/fx/cx/v21/s033/cloc/raw/filtered/iteration-001.log`부터 `iteration-100.log`
- 실행 스크립트 사본: `/root/jobs/fx/cx/v21/s033/cloc/scripts/`

코드 변경이 없어 리팩토링 점검은 해당하지 않는다. 테스트 변경은 없으며 남은 test failure는 없다.
