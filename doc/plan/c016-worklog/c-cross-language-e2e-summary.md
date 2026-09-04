# §C cross-language E2E 검증 요약

검증은 C++ full build의 첫 실제 실패에서 중단됐다. 실패는 Core/runtime 동작이 아니라 Bingo protobuf 생성 경로와 compile include 경로가 일치하지 않는 build configuration 문제다. 요청한 수정 허용 경로 밖의 `framework/languages/cpp/CMakeLists.txt`를 바꾸지 않았고, 후속 cross-language E2E와 sample은 실행하지 않았다.

## stale C++ host 조사

`.NET` 디렉터리에는 별도의 cross-language runner가 없다. `.NET` TestHost를 피어로 실행하고 C++ host 위치를 결정하는 유일한 진입점은 `framework/languages/cpp/cross-language/run_cross_language_smoke.sh`이다.

- `run_cross_language_smoke.sh:14-15`는 `BUILD_DIR="${ZLINK_CPP_BUILD_DIR:-${CPP_ROOT}/build-redis-vcpkg}"`와 `${BUILD_DIR}/zlink_cpp_cross_language_host`를 사용한다.
- 따라서 기존 0.13.2 host를 사용한 실행은 caller가 `ZLINK_CPP_BUILD_DIR`를 전달하지 않아 legacy default build tree를 선택한 경우와 일치한다. 현재 tree의 runner에는 이미 환경변수 우선 계약이 있어 script 변경은 필요하지 않았다.
- 지정 build cache는 `ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION=0.17.0`, `ZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION=0.17.0`, `zlink_cpp_DIR=.artifacts/wsl/install/zlink-cpp/0.17.0/...`, `zlink_DIR=.artifacts/wsl/install/zlink-core/0.17.0/...`를 가리켰다.
- 하지만 full build가 host link 전에 실패했고, 지정 build dir에 `zlink_cpp_cross_language_host`가 생성되지 않았다. 따라서 0.17 host 실행 확인은 못 했다.

예정한 정확한 runner 적용은 다음과 같았다. 이 실행은 build blocker 때문에 도달하지 못했다.

```bash
ZLINK_CPP_BUILD_DIR=/home/hep7/project/zlink/framework/languages/cpp/build/linux-ninja-c-e2e \
ZLINK_CPP_CROSS_KEEP_RUN_DIR=1 \
framework/languages/cpp/cross-language/run_cross_language_smoke.sh
```

## 실행 결과

### Cross-language

| 범위 | 상태 | exit | 시간 | 근거 |
|---|---:|---:|---:|---|
| C++ full build 선행 단계 | FAIL | 1 | 153.79s | `bingo_messages.pb.h: No such file or directory` |
| C++ 진입점, all stages | NOT RUN | - | - | 선행 build 실패, 0.17 C++ host 미생성 |
| .NET 피어 stages | NOT RUN | - | - | 별도 runner 없음; C++ all-stage runner에 포함 |
| Node `run_cross_language_smoke.sh` | NOT RUN | - | - | run order의 선행 build 실패 |
| Java↔C++ stages | NOT RUN | - | - | C++ all-stage runner에 포함되며 0.17 host 미생성 |
| `java-cross` selector | NOT RUN | - | - | run order의 선행 build 실패; 현재 runner 주석상 이 selector는 Java↔Node/.NET만 실행 |

### C++ samples

| sample | 상태 | exit | 시간 |
|---|---:|---:|---:|
| TicTacToe | NOT RUN | - | - |
| Bingo | NOT RUN | - | - |
| DeliveryDispatch | NOT RUN | - | - |
| SupportChat | NOT RUN | - | - |
| GameQuest | NOT RUN | - | - |
| ShoppingMall | NOT RUN | - | - |
| ZoneWorld | NOT RUN | - | - |

`run_samples.sh`는 full build 완료 후 재빌드 경쟁 없이 실행하라는 조건을 만족하지 못해 실행하지 않았다.

## 실패 분류

| bucket | 결과 | 근거 |
|---|---|---|
| A: DONTWAIT/backpressure | 관찰 없음 | runtime/E2E까지 진입하지 못함 |
| B: terminal/error classification | 관찰 없음 | runtime/E2E까지 진입하지 못함 |
| C: known pre-existing | 관찰 없음 | 해당 test/lint/browser/Java gate를 실행하지 않음 |
| D: environment/runner | FAIL | build dir에는 `samples/Bingo/Shared/Contracts/bingo_messages.pb.h`가 생성됐지만 compile은 build root만 `-I`로 추가함 |

`framework/languages/cpp/samples/Bingo/Shared/Contracts/messages.hpp:4`는 `#include "bingo_messages.pb.h"`를 요구한다. `framework/languages/cpp/CMakeLists.txt:623-632`는 protobuf output dir을 build root로 지정하고 build root만 public include dir로 추가하지만, 현재 `protobuf_generate` 결과는 source 상대 경로를 유지한 하위 디렉터리에 생성됐다. 실패 compile command에는 해당 하위 디렉터리 include가 없다. 이는 요청에서 허용한 runner/script 수정 범위 밖이다.

## 실행한 명령

```bash
git branch --show-current
git status --short

mkdir -p zlink-work/c016/logs /dev/shm/zlink-tmp-cpp
/usr/bin/time -f '\nwall_seconds=%e\nexit_status=%x' \
  cmake --build framework/languages/cpp/build/linux-ninja-c-e2e -j16

# 실패 후 read-only 진단
find framework/languages/cpp/build/linux-ninja-c-e2e \
  -name 'bingo_messages.pb.h' -o -name 'zlink_cpp_cross_language_host'
ninja -C framework/languages/cpp/build/linux-ninja-c-e2e -t query \
  CMakeFiles/test_cpp_framework_sample_parity.dir/tests/Zlink.Framework.ContractTests/test_cpp_framework_sample_parity.cpp.o
```

로그:

- `zlink-work/c016/logs/c-e2e-01-cpp-build.log`
- `zlink-work/c016/logs/c-e2e-02-diagnostics.log`

## BLOCKERS

1. `framework/languages/cpp/CMakeLists.txt:623-632`의 protobuf generated-header 경로 계약을 수정해 full build를 완료해야 한다. 수정 후에는 지정 build dir에서 `zlink_cpp_cross_language_host`가 새로 link되었는지 확인해야 한다.
2. 0.17 C++ host가 없으므로 C++/.NET/Node/Java all-stage E2E와 Java↔C++ 방향을 검증할 수 없다.
3. full build 완료 전이므로 7개 C++ sample을 재빌드 없이 격리 실행하는 조건을 만족할 수 없다.
