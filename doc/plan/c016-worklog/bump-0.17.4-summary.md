# bump-0.17.4 결과

## 결과

- `0.17.3`에서 `0.17.4`로 Core와 bindings의 현재 배포 버전을 올렸다.
- 기준은 `0761c1d4d0`의 변경 범위와 Rust 헤더 보완 커밋 `4cdafee9b7`이다. `framework/**`와 `core/CHANGELOG.md`는 변경하지 않았다.
- Debian은 새 `0.17.4-0.1` 항목을 맨 앞에 추가했다. NuGet·Red Hat·vcpkg는 현재 version 필드를 갱신했다.
- Node `package-lock.json`은 root package의 두 version 필드만, 각 Rust `Cargo.lock`은 `zlink` package version만 변경했다.
- `git diff --check`를 통과했다. 커밋·태그는 만들지 않았다.

## 헤더 mirror

`core/include/zlink.h` 및 `core/include/zlink/common.h`와 byte-for-byte 비교했다.

| 언어 | zlink.h | zlink/common.h |
| --- | --- | --- |
| C | identical | identical |
| C++ | identical | identical |
| Go | identical | identical |
| Rust | identical | identical |

따라서 4개 언어의 8개 번들 헤더 모두 Core의 `ZLINK_VERSION_MAJOR/MINOR/PATCH` 상수와 일치하며, Rust `common.h`의 PATCH도 4다.

## 검증

| 명령 | 결과 |
| --- | --- |
| `JOBS=4 scripts/build-core.sh dev` | 성공. CMake가 `0.17.4`를 감지했다. |
| `ctest --test-dir core/build-dev -R 'version|contract_surface|header' --output-on-failure` | 성공, 2/2. |
| `JOBS=4 scripts/build-core.sh release --lib-only` | 성공. `core/build/lib/libzlink.so.0.17.4` 생성, SONAME은 `libzlink.so.0`. |
| `ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=4 bindings/c/tests/run_tests.sh` | 성공. contract 10/10, sample smoke 6/6. |
| `ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=4 bindings/cpp/tests/run_tests.sh` | 성공. contract 19/19, sample smoke 7/7. |
| Rust local `cargo build -vv` | 미완료. 설치된 Cargo 1.75.0이 manifest의 Rust 2024 edition을 파싱하지 못해 build script 전에 종료했다. |

Rust `build.rs`는 local Core 헤더의 `MAJOR/MINOR/PATCH`를 읽고 `libzlink.so.<core_version>`를 검사하도록 구현되어 있다. local 환경을 올바르게 export한 명령은 실행했지만, 위 도구체인 제약 때문에 실제 build.rs 로그에서 `libzlink.so.0.17.4` 검증까지 도달하지 못했다. Rust 1.85 이상 Cargo로 동일 명령을 재실행해야 한다.

## 남은 `0.17.3` 참조

요청한 grep은 `.git`을 제외하지 않으므로 VCS reflog, branch configuration, linked-worktree metadata 및 binary index도 함께 출력한다. 이들은 모두 역사/VCS 메타데이터이며 작업 범위 밖이다. working-tree의 텍스트 참조는 다음뿐이다.

| 파일 | 분류 | 판단 |
| --- | --- | --- |
| `vcpkg/ports/zlink/portfile.cmake` | 의도적 이전 source REF | `core/v0.17.3` release source ref이며 vcpkg의 현재 version field가 아니다. |
| `scripts/gate/common.sh` | 예시 경로 | cache directory 주석의 이전 version 예시다. |
| `core/packaging/debian/changelog` | 역사 | 새 항목 아래의 이전 Debian release 기록이다. |
| `core/packaging/conan/conandata.yml` | 역사/URL | 이전 release source archive의 key와 URL이다. |
| `core/CHANGELOG.md` | 역사 | 기존 release notes와 `[0.17.3]` section이다. 지시에 따라 미변경이다. |
| `bindings/node/provenance/core-package-provenance.json` | 의도적 provenance | 공개된 이전 Core package의 version, runtime path, tag를 고정한 provenance다. |

## 설계 및 분류

- 비교: 저장소 전체의 모든 과거 문자열을 바꾸는 방식은 release provenance와 history를 훼손한다. 기준 커밋의 현재 version field 및 동기화된 헤더만 갱신하는 방식을 선택했다.
- 규칙 수: 전후 모두 각 배포 계층은 현재 version field 하나를 소유한다. 새 상태, helper, 옵션, 동작 규칙은 추가하지 않았다.
- 변경 분류: A — 계약 적응(공개 버전 식별자와 패키지 메타데이터의 patch release 갱신). Runtime 동작과 공개 ABI 표면은 변경하지 않았다.

## `git diff --stat` 전체

```text
 BINDINGS_VERSION                                   |  2 +-
 VERSION                                            |  2 +-
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
 bindings/go/include/zlink.h                        |  2 +-
 bindings/go/include/zlink/common.h                 |  2 +-
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
 .../fixtures/public-consumer/PublicConsumer.csproj |  2 +-
 .../node/fixtures/public-consumer/package.json     |  2 +-
 vcpkg/ports/zlink/vcpkg.json                       |  2 +-
 43 files changed, 112 insertions(+), 106 deletions(-)
```
