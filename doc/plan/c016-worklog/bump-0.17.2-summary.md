# bump-0.17.2 결과

## 결과

`libzlink`와 binding package 버전을 0.17.1에서 0.17.2로 올렸다. `VERSION`과
`BINDINGS_VERSION`은 모두 0.17.2이며 release build는
`core/build/lib/libzlink.so.0.17.2`를 생성했다. `framework/**`는 변경하지 않았다.

`4cd03b9173`의 파일 목록은 framework·doc을 제외하면 42개다(브리프의 40개와 달리
마지막의 local-package .NET/Node 소비자 fixture 2개가 포함됨). 이 두 파일은 모두
패키지 버전을 pin하는 source version이므로 함께 올렸다.

## 변경 파일

- Root/Core: `BINDINGS_VERSION`, `VERSION`, `core/CMakeLists.txt`,
  `core/include/zlink.h`, `core/include/zlink/common.h`.
- Core packaging: `core/packaging/debian/changelog`, `zlink.dsc`, NuGet의
  `package.config`, `package.nuspec`, `package.targets`, Red Hat의 `zlink.spec`.
  Debian changelog는 기존 0.17.1 UNRELEASED 기록을 유지하고 0.17.2 항목을 위에
  추가했다. NuGet 파일 이름의 `0_17_1`도 `0_17_2`로 일괄 변경했다.
- C/C++: 각 `include/zlink.h`, `include/zlink/common.h`, C의 두 version/contract
  test, C++ `CMakeLists.txt`, C++ header version contract test.
- .NET: `Zlink.csproj`, `Runtime/Native/NativeLibraryLoader.cs`.
- Go: `include/zlink.h`, `include/zlink/common.h`, raw Core allowlist와 검사.
- Java: `build.gradle`, `tests/run_tests.sh`.
- Node: `package.json`, `package-lock.json`, `scripts/resolve_core.js`,
  `scripts/verify_prebuilds.js`.
- Python: `pyproject.toml`, `setup.py`, native loader.
- Rust: `Cargo.toml`, 세 Cargo.lock, 두 header mirror.
- local-package 소비자 pin: `scripts/local-package/dotnet/fixtures/public-consumer/PublicConsumer.csproj`,
  `scripts/local-package/node/fixtures/public-consumer/package.json`.

버전 검증은 (1) root manifest만 바꾸는 방법과 (2) public header mirror, contract test,
package metadata, consumer pin을 함께 바꾸는 방법을 비교했다. (2)를 선택했다. 각
언어가 Core 0.17.2와 일치하는지를 build/contract test에서 검증할 수 있고, 별도 우회
규칙이나 runtime 상태를 추가하지 않는다. runtime 동작 변경은 없다.

## 검증

- `JOBS=4 scripts/build-core.sh dev`: 통과. CMake가 `0.17.2`를 감지했다.
- `ctest --test-dir core/build-dev -R 'version|contract_surface|header' --output-on-failure`:
  2/2 통과 (`contract_c_header_mirror`, `unittest_public_contract_headers`).
- `JOBS=4 scripts/build-core.sh release --lib-only`: 통과.
  `core/build/lib/libzlink.so.0.17.2` 존재를 확인했다.
- `ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=4 bindings/c/tests/run_tests.sh`:
  contract 10/10, sample smoke 6/6 통과.
- `ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=4 bindings/cpp/tests/run_tests.sh`:
  header version contract는 통과했으나 전체 contract 중 1개가 실패했다.
  `test_cpp_contract_socket`의
  `tests/contract/test_cpp_contract_socket.cpp:773`에서 concurrent multipart의
  `invalid_argument` 기대 assertion이 실패했다. 버전 변경과 무관한 Core runtime
  contract 실패이므로 test expectation과 runtime 코드는 변경하지 않았다. 나머지
  18/19 contract test는 통과했다.
- 선택 검증 Python: `bindings/python/tests/run_tests.sh tests/test_version.py`는
  시스템 Python에 `pytest`가 없어 실행하지 못했다.
- 선택 검증 Go: targeted `go test`는 `bindings/go/native/linux-x86_64`의 고정 native
  library를 링크하며 새 Core 심볼(`zlink_completion_close`, `zlink_reply_part` 등)을
  찾지 못해 링크 단계에서 실패했다. local Core runtime의 버전 문제를 test expectation
  변경으로 우회하지 않았다.
- `git diff --check`: 통과.

## 남은 0.17.1 참조와 판단

브리프의 제외 조건과 같은 범위(`framework/**`, `doc/**`, build directory 제외)에서
남은 `0.17.1`은 `core/packaging/debian/changelog`의 직전 0.17.1 UNRELEASED 항목뿐이다.
이는 Debian 변경 이력이며 현재 package version이 아니므로 유지했다. 모든 `doc/**` 참조는
과거 작업·성능·bug 기록이고 source version이 아니어서 유지했다. framework의 binding
참조는 브리프 지시대로 변경하지 않았다. 새 source-version 의미의 0.17.1 참조는 발견하지
못했다.

## 전체 `git diff --stat`

아래는 보고서 작성 직전의 전체 tracked diff다. `doc/plan/c016-worklog/briefs/bump-0.17.2.prompt`
한 줄은 작업 시작 전에 이미 존재한 감독관 변경이며 이 작업에서 수정하지 않았다. 이 보고서와
progress 파일은 untracked이므로 `git diff --stat`에 포함되지 않는다.

```text
 BINDINGS_VERSION                                   |  2 +-
 VERSION                                            |  4 +-
 bindings/c/include/zlink.h                         |  2 +-
 bindings/c/include/zlink/common.h                  |  2 +-
 bindings/c/tests/test_c_common_header_version.c    |  4 +-
 bindings/c/tests/test_c_contract_surface.c         |  4 +-
 bindings/cpp/CMakeLists.txt                        |  8 ++--
 bindings/cpp/include/zlink.h                       |  2 +-
 bindings/cpp/include/zlink/common.h                |  2 +-
 .../test_cpp_contract_common_header_version.cpp    |  4 +-
 .../Zlink/Runtime/Native/NativeLibraryLoader.cs    |  2 +-
 bindings/dotnet/src/Zlink/Zlink.csproj             | 16 +++----
 bindings/go/include/zlink.h                         |  2 +-
 bindings/go/include/zlink/common.h                  |  2 +-
 bindings/go/internal/native/raw_contract_test.go   |  2 +-
 bindings/go/tests/raw-core11-allowlist.json        |  2 +-
 bindings/java/build.gradle                         | 14 +++---
 bindings/java/tests/run_tests.sh                   |  2 +-
 bindings/node/package-lock.json                    |  4 +-
 bindings/node/package.json                         |  2 +-
 bindings/node/scripts/resolve_core.js              |  4 +-
 bindings/node/scripts/verify_prebuilds.js          |  2 +-
 bindings/python/pyproject.toml                     |  2 +-
 bindings/python/setup.py                           |  2 +-
 .../python/src/zlink/_native/_native_loader.py     |  2 +-
 bindings/rust/Cargo.lock                           |  2 +-
 bindings/rust/Cargo.toml                           |  2 +-
 bindings/rust/include/zlink.h                      |  2 +-
 bindings/rust/include/zlink/common.h               |  2 +-
 bindings/rust/perf/multi/Cargo.lock                |  2 +-
 bindings/rust/perf/single/Cargo.lock               |  2 +-
 core/CMakeLists.txt                                |  2 +-
 core/include/zlink.h                               |  2 +-
 core/include/zlink/common.h                        |  2 +-
 core/packaging/debian/changelog                    |  6 +++
 core/packaging/debian/zlink.dsc                    |  2 +-
 core/packaging/nuget/package.config                |  2 +-
 core/packaging/nuget/package.nuspec                | 50 +++++++++++-----------
 core/packaging/nuget/package.targets               | 40 ++++++++---------
 core/packaging/redhat/zlink.spec                   |  2 +-
 doc/plan/c016-worklog/briefs/bump-0.17.2.prompt    |  1 +
 .../fixtures/public-consumer/PublicConsumer.csproj |  2 +-
 .../node/fixtures/public-consumer/package.json     |  2 +-
 43 files changed, 113 insertions(+), 106 deletions(-)
```

커밋과 태그는 만들지 않았다.
