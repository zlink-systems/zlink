# Local Package 스크립트

이 디렉터리는 framework가 bindings 소스를 직접 참조하지 않고, 로컬에서 만든 배포 패키지를
참조하도록 준비하는 스크립트를 둔다.

## 정책

- 기본 산출물은 `.artifacts/wsl` 또는 `.artifacts/windows` 아래에 만든다.
- `ZLINK_LOCAL_PACKAGE_ROOT` 환경 변수로 산출물 위치를 바꿀 수 있다.
- WSL과 Windows 산출물은 섞지 않는다.
- local package 생성 스크립트는 이 디렉터리에만 둔다. `bindings/<lang>/` 아래에는 언어별
  package 설정만 두고, 로컬 배포 실행 스크립트를 중복해서 만들지 않는다.
- 스크립트는 bindings package 생성만 담당한다. framework가 package mode로 빌드되는지 검증하는
  스크립트는 별도로 둔다.
- source reference는 bindings 개발 중에만 명시적으로 켜는 예외 경로로 유지한다.
- core native library를 bindings workspace에 동기화하는 스크립트도 이 디렉터리 아래에서만 관리한다.
- 동기화 대상을 제한해야 하면 binding 이름을 인자로 넘긴다. 인자를 생략하면 기존과 같이 모든
  binding workspace를 동기화한다.
- framework는 bindings 소스를 직접 참조하지 않고, 명시한 버전의 local package만 참조한다.
  bindings 새 버전을 local package 위치에 배포해도 framework의 참조 버전을 바꾸기 전까지는
  기존 버전을 계속 사용해야 한다.
- Core 버전과 binding package 버전은 서로 다른 값이다. Core가 `10.2.0`인 상태에서 .NET binding만
  수정했다면 .NET package는 `10.2.1`로 만들 수 있고, package 안의 native library는 계속
  `libzlink.so.10.2.0`을 사용한다. 다른 binding package는 `10.2.0`을 유지한다.
- Core가 수정되면 Core의 minor를 올리고 patch는 0으로 둔다. 모든 binding은 새 Core의
  `major.minor`를 따르는 `10.x.0`에서 다시 시작한다. 그 뒤 binding에만 수정이 생겼을 때 해당
  binding의 patch만 올린다.

역할은 다음처럼 나눈다.

```bash
./scripts/local-package/native/sync-local-core-libs.sh cpp dotnet java node
```

```text
bindings/<lang>/           package metadata and build rules
scripts/local-package/     local package output policy and commands
```

이렇게 나누면 NuGet, Maven, npm, CMake package 정의는 각 bindings 프로젝트에 남고,
`.artifacts/wsl`과 `.artifacts/windows`로 배포하는 로컬 개발 정책은 한 곳에서 관리된다.

## 디렉터리 구조

```text
scripts/local-package/
  build-wsl.sh
  build-windows.ps1
  publish-all-wsl.sh
  core/
    build-wsl.sh
    test-build-wsl.sh
  native/
    update-zlink-libs.sh
    sync-local-core-libs.sh
    fetch-release-binaries.sh
  cpp/
  dotnet/
  go/
  rust/
  java/
  node/
```

Core 11 package는 bindings package와 별도로 만든다. 이미 build한 Core candidate만 versioned install
prefix에 설치하며 외부로 배포하지 않는다. repository `VERSION`, CMake build version, package manifest,
설치된 runtime의 `zlink_version()`은 모두 같은 `11.x.y`여야 한다. 값이 다르면 package를 만들지 않는다.

```bash
./scripts/local-package/core/test-build-wsl.sh --self-test --dry-run \
  --evidence /absolute/path/core-package-tooling-self-test.json
./scripts/local-package/core/build-wsl.sh --build-dir core/build \
  --output-root /absolute/path/local-package-root \
  --candidate-manifest /absolute/path/V11-M3-CORE-VERIFY.json \
  --review-evidence /absolute/path/V11-R2-result.json \
  --evidence /absolute/path/core-package.json
```

Core package 기본 install 구조는 `<output-root>/install/zlink-core/<version>/`이다. Core 11 package에는
raw C header와 runtime만 포함하며 `include/zlink/service/`는 포함하지 않는다. clean C consumer는 package
prefix 밖의 header와 library 환경 변수를 제거한 상태에서 compile, link, load하고 runtime version까지
manifest와 비교한다. Package 생성과 consumer 검증은 review를 통과한 `V11-M3-CORE-VERIFY` candidate
manifest의 absolute path를 반드시 받는다. Candidate의 base revision, 현재 Core 파일 hash와 aggregate hash를
다시 검증하고 이 값을 package provenance에 기록하므로, commit하지 않은 candidate를 기존 `HEAD`에서 만든
산출물로 잘못 표시하지 않는다. 또한 `V11-R2`가 `passed`로 기록된 review evidence를 함께 받고, 그 문서가
승인한 candidate manifest SHA-256과 package 입력이 정확히 같은지 확인한다.

Candidate의 direct input은 review 시점의 spec·ledger provenance를 봉인한다. Review가 진행되면 ledger status와
증거 문구는 정상적으로 바뀌므로 package 단계에서는 direct input의 schema·path·봉인 hash 형식만 검증한다.
반면 candidate files와 Core changed path는 현재 worktree의 내용·mode·base hash와 계속 정확히 같아야 한다.
이 구분으로 review 이후 ledger 갱신은 허용하면서 reviewed source나 tooling 변경은 계속 거부한다.

`output-root`, candidate manifest와 evidence 경로는 dot segment가 없는 canonical absolute path여야 한다.
Script는 삭제할 install prefix가 canonical output root 아래인지 확인한 뒤 해당 version directory만 교체한다.

## WSL 사용법

Core release를 binding이 포함하는 native library와 Core 계약 표시에 반영하고 모든 local package를
한 번에 만든다.
첫 번째 인자로 release tag 또는 release URL을 넘긴다. 스크립트는 native library와
Core version marker를 먼저 맞춘 뒤 .NET, Java/Kotlin, Node.js, C++ package를 차례로 만든다.
각 binding의 package version은 바꾸지 않는다.

```bash
./scripts/local-package/publish-all-wsl.sh core/v9.0.0
```

RC를 검증할 때는 asset tag의 `-rc.N`을 유지한다. 다만 Core의 C header와 runtime이 보고하는 version은
항상 숫자 `X.Y.Z`이며 RC suffix를 포함하지 않는다.

```bash
./scripts/local-package/publish-all-wsl.sh core/v10.7.0-rc.1
```

동기화 단계는 release의 `release-provenance.txt`를 읽어 tag가 가리키는 source commit, 숫자 runtime
version, `checksums.txt`와 source archive의 SHA-256, source asset URL을 함께 검증한다. 어느 값이라도
다르면 binding workspace를 갱신하기 전에 실패한다. stable과 RC fixture 및 실패 조건은 다음 명령으로
로컬에서 확인한다.

```bash
./scripts/local-package/native/test-release-contract.sh
```

release repository를 명시하거나 기대 버전을 검증하려면 다음처럼 실행한다.

```bash
./scripts/local-package/publish-all-wsl.sh core/v9.0.0 \
  --repo kairos-code-dev/zlink \
  --expect-version 9.0.0
```

이 명령은 binding workspace의 native 파일과 Core version marker를 변경한다. framework의 bindings
참조 버전은 변경하지 않으므로, package를 검증한 뒤에는 아래의 중앙 버전 위치를 별도로 갱신해야 한다.

binding만 patch release할 때는 해당 binding의 package version만 먼저 올리고 필요한 package만 만든다.
예를 들어 Core `10.2.0`을 그대로 사용하는 .NET binding `10.2.1`은 다음 순서로 준비한다.

```bash
./scripts/local-package/native/update-zlink-libs.sh core/v10.2.0 \
  --repo kairos-code-dev/zlink \
  --expect-version 10.2.0
./scripts/local-package/build-wsl.sh dotnet
```

첫 번째 명령은 GitHub Core release의 `checksums.txt`로 다운로드한 파일을 검증하고, native runtime의
`zlink_version`이 `10.2.0`과 정확히 같은지도 확인한다. .NET package version `10.2.1`은
`bindings/dotnet/src/Zlink/Zlink.csproj`가 소유하며 Core 파일명으로 사용하지 않는다.

현재 bindings 버전으로 전체 package만 다시 만들 때는 기존 wrapper를 사용한다.

전체 bindings package를 만든다.

```bash
./scripts/local-package/build-wsl.sh
```

언어를 지정해서 만들 수도 있다.

```bash
./scripts/local-package/build-wsl.sh dotnet node
```

기본 산출물 위치는 다음과 같다.

```text
.artifacts/wsl/
  nuget/
  maven/
  npm/
  install/zlink-cpp/<version>/
```

## Windows 사용법

PowerShell에서 실행한다.

```powershell
.\scripts\local-package\build-windows.ps1
```

언어를 지정해서 만들 수도 있다.

```powershell
.\scripts\local-package\build-windows.ps1 dotnet node
```

기본 산출물 위치는 다음과 같다.

```text
.artifacts/windows/
  nuget/
  maven/
  npm/
  install/zlink-cpp/<version>/
```

## 언어별 산출물

| 언어 | 스크립트 | 산출물 |
|------|----------|--------|
| .NET | `dotnet/build-wsl.sh`, `dotnet/build-windows.ps1` | NuGet package |
| Go | `go/build-wsl.sh` | Go file proxy module |
| Rust | `rust/build-wsl.sh` | Cargo crate와 candidate-bound clean consumer evidence |
| Java/Kotlin | `java/build-wsl.sh`, `java/build-windows.ps1` | Maven repository layout |
| Node.js | `node/build-wsl.sh`, `node/build-windows.ps1` | npm tarball |
| C++ | `cpp/build-wsl.sh`, `cpp/build-windows.ps1` | 버전별 CMake install prefix |

### Rust candidate package

Rust crate는 승인된 Core candidate와 설치된 Core provenance를 입력으로 받아야 한다. 다음 명령은 Linux
x86_64 payload만 포함한 현재 package version의 crate를 만들고, package contents, workspace test, clippy, samples와
path dependency 없는 clean consumer를 같은 candidate 기준으로 검증한다.

```bash
scripts/local-package/rust/build-wsl.sh \
  --platforms linux-x86_64 \
  --core-candidate-manifest /absolute/path/candidate-reply-match-completion-hwm-20260801.json \
  --core-package-evidence /absolute/path/core-package-20260801.json \
  --output-root /absolute/path/.artifacts/wsl/rust-candidate
```

`bindings/rust/perf/run_benchmarks.sh`, `run_benchmarks_multi.sh`와 `tests/run_tests.sh`는 각각
`--rust-package-evidence FILE` 또는 `ZLINK_RUST_PACKAGE_EVIDENCE`를 받을 수 있다. 내부 resolver는 package
evidence의 source revision, candidate manifest와 runtime SHA-256, clean consumer 상태를 다시 확인한 뒤
`ZLINK_RUST_NATIVE_DIR`와 runtime identity를 설정한다. 따라서 perf·source test가 다른 `core/build` runtime을
조용히 선택하지 않는다.

Clean consumer evidence의 `LD_LIBRARY_PATH=unset` 조건은 package 내부 runtime load를 확인한다. 현재 Cargo
dependency의 build-script linker argument는 downstream consumer에 자동 전파되지 않으므로, 이 local gate는
package source에서 계산한 RPATH를 `RUSTFLAGS`로 주입한다. 이 조건은 evidence에
`linkerRpath: package-derived-rustflags`로 기록되며, 일반 crate 사용자가 별도 설정 없이 RPATH를 얻는다는
의미로 해석하지 않는다.

## HTTP client 트랙 (framework component)

bindings 외에 framework HTTP client도 같은 정책으로 local package를 만든다.
e2e/sample 소비자는 http-client 소스가 아니라 고정 버전 local package를 참조하므로,
client를 수정해도 소비자 참조 버전을 올리기 전까지는 영향이 없다.

```bash
./scripts/local-package/http-client/build-wsl.sh          # dotnet java node
```

| 언어 | 산출물 | 소비자 참조 버전 위치 |
|------|--------|----------------------|
| .NET | `nuget/Systems.Zlink.Stream.Connector.<ver>.nupkg`, `nuget/Zlink.Framework.Contracts.<ver>.nupkg`, `nuget/Zlink.HttpClient.<ver>.nupkg` | `framework/languages/dotnet/Directory.Packages.props`의 `ZLinkHttpClientPackageVersion` |
| Java | `maven/systems/zlink/zlink-http-client/<ver>/` | (소비자 없음 — 게시만) |
| Node.js | `npm/zlink-systems-http-client-<ver>.tgz` | `framework/languages/node/package.json`의 `@zlink-systems/http-client` tarball 경로 |
| C++ | 제외 | static lib + in-tree framework PUBLIC 헤더 의존이라 설치본/소스 혼합 시 ODR 위험. cpp 소비자는 소스 참조를 유지하고 `test_cpp_http_client` 계약 테스트로 게이트한다 |

버전 정본: dotnet `Zlink.HttpClient.csproj`의 `<Version>`, java
`HttpClientVersion.java`(gradle 버전이 이 파일에서 파생), node
`packages/http-client/package.json`, cpp `contracts/types.hpp`의 `version_*` 상수.
네 곳 모두 User-Agent `zlink-http-client/<major.minor>`가 버전에서 파생된다.

## Framework 참조 버전 관리

framework가 사용하는 bindings 버전은 언어별로 한 곳에서 바꾼다. 이 값은 자동으로 최신 버전을
따라가기 위한 값이 아니라, framework가 검증한 bindings 버전을 고정하기 위한 값이다.

| 언어 | framework 참조 버전 변경 위치 | 참조 방식 |
|------|-------------------------------|-----------|
| .NET | `framework/languages/dotnet/Directory.Packages.props`의 `ZLinkBindingsPackageVersion` | `Systems.Zlink` NuGet package version |
| Java/Kotlin | `framework/languages/java/gradle/libs.versions.toml`의 `zlinkBindings` | `zlinkLibs.zlink.bindings` Gradle version catalog |
| Node.js | `framework/languages/node/package.json`의 `@zlink-systems/zlink` | versioned local npm tarball |
| C++ | `framework/languages/cpp/CMakeLists.txt`의 `ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION` | `find_package(zlink_cpp <version> EXACT CONFIG REQUIRED)` |

버전을 올릴 때는 먼저 bindings package를 새 버전으로 만든 뒤, 위 표의 framework 참조 버전을
같은 버전으로 바꾼다. 이렇게 해야 local package 저장소에 여러 버전이 있어도 framework가 뜻하지
않게 최신 버전을 잡지 않는다.

Java/Kotlin은 여러 sample과 e2e가 별도 Gradle build로도 실행된다. 그래서 각 `build.gradle.kts`에
직접 `systems.zlink:zlink:<version>`을 쓰지 않고, 공통 version catalog의 `zlinkLibs.zlink.bindings`를
사용한다. Node.js는 npm package manifest에서 변수를 직접 쓸 수 없으므로 workspace root의
`package.json`에만 local tarball 경로를 둔다. 각 workspace package는 root dependency로 설치된
`@zlink-systems/zlink`를 사용한다.

Node.js package를 나중에 각각 npm registry에 독립 배포하려면 publish 단계에서 각 package manifest에
registry용 `@zlink-systems/zlink` dependency를 주입하거나, 배포용 manifest를 별도로 생성해야 한다.
local framework 개발에서는 root `package.json`의 versioned tarball 경로가 기준이다.

## 환경 변수

| 이름 | 의미 |
|------|------|
| `ZLINK_LOCAL_PACKAGE_ROOT` | local package 산출물 root |
| `CONFIGURATION` | `.NET`과 C++ 빌드 configuration. 기본값은 `Release` |
| `ZLINK_SKIP_NPM_CI` | `true`이면 Node package 생성 전에 `npm ci`를 건너뛴다 |
| `ZLINK_NODE_PACKAGE_MODE` | Node package 정책. `source`는 source-build tarball, `prebuild`는 pack 전에 prebuild를 검증한다 |
| `ZLINK_CORE_VERSION` | Node prebuild 검증에 사용할 Core version. 저장소에서는 `VERSION`과 정확히 같아야 한다 |
| `ZLINK_CPP_LOCAL_BUILD_DIR` | C++ bindings build directory. 기본값은 버전별 디렉터리다 |
| `ZLINK_CPP_INSTALL_PREFIX` | C++ bindings install prefix. 기본값은 `install/zlink-cpp/<version>`이다 |

## Native 라이브러리 동기화

core release 산출물이나 로컬 core 빌드 결과를 bindings workspace에 반영하는 스크립트는
`scripts/local-package/native/`에 둔다.

| 스크립트 | 용도 |
|----------|------|
| `native/update-zlink-libs.sh` | GitHub release asset을 받아 bindings native library와 버전 마커를 갱신한다 |
| `native/sync-local-core-libs.sh` | 로컬 `core/build/lib` 결과를 bindings native 위치에 복사한다 |
| `native/fetch-release-binaries.sh` | release asset 다운로드와 파일 복사를 수행하는 내부 스크립트 |
| `native/test-release-contract.sh` | RC/stable tag와 provenance 실패 조건을 fixture로 검증한다 |

기존에 `bindings/` 또는 `core/tools/` 아래에 있던 동기화 스크립트는 유지하지 않는다. 자동화와
문서는 이 표의 경로를 직접 호출해야 한다.

`native/update-zlink-libs.sh`의 `--expect-version`은 binding package version이 아니라 Core version이다.
release tag, checksum으로 검증한 asset, native `zlink_version`이 이 값과 정확히 일치해야 한다.
package version은 각 binding의 정식 metadata에서 따로 관리한다.

`.NET` 바인딩의 native library 기준 위치는 `bindings/dotnet/native/<rid>/`이다. NuGet package를
만들 때는 이 파일들이 `runtimes/<rid>/native/`로 들어간다. `bindings/dotnet/runtimes/`는 더 이상
소스 저장소에서 직접 관리하는 입력 위치로 사용하지 않는다.

## 다음 단계

이 스크립트가 만든 산출물을 framework build가 참조하도록 바꾼다.

- `.NET`: `.artifacts/<env>/nuget`을 NuGet source로 추가한다.
- Java/Kotlin: `.artifacts/<env>/maven`을 Maven repository로 추가한다.
- Node.js: `.artifacts/<env>/npm/*.tgz`를 설치한다.
- C++: `.artifacts/<env>/install/zlink-cpp/<version>`를 `CMAKE_PREFIX_PATH`에 넣고 `find_package(... EXACT)`로 참조한다.

## 검증

local package를 새로 만들거나 framework 참조 버전을 바꾼 뒤에는 사용하는 언어만 골라서 검증한다.
e2e가 필요한 작업이 아니라면 unit 또는 package 해석 확인만 실행한다.

```bash
# .NET
dotnet test framework/languages/dotnet/Zlink.Framework.sln --filter FullyQualifiedName~UnitTests

# Java/Kotlin root build
cd framework/languages/java
./gradlew --no-daemon test

# Java/Kotlin 별도 e2e Gradle build의 설정 확인 예
cd framework/languages/java/e2e/SpotService
../../gradlew --no-daemon help

# Node.js
cd framework/languages/node
npm run build
npm run typecheck
node --test test/smoke/binding-smoke.test.js \
  test/contract/native-artifact-freshness.test.js \
  test/contract/node-binding-parity.test.js

# C++
cmake -S framework/languages/cpp -B .artifacts/wsl/build/framework-cpp-local-package-tests \
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=ON \
  -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_E2E=OFF
ctest --test-dir .artifacts/wsl/build/framework-cpp-local-package-tests \
  -L framework-unit --output-on-failure
```

## 로컬 배포 위치

기본값 기준으로 언어별 bindings local package 위치는 다음과 같다.

| 환경 | .NET | Java/Kotlin | Node.js | C++ |
|------|------|-------------|---------|-----|
| WSL | `.artifacts/wsl/nuget` | `.artifacts/wsl/maven` | `.artifacts/wsl/npm` | `.artifacts/wsl/install/zlink-cpp/<version>` |
| Windows | `.artifacts/windows/nuget` | `.artifacts/windows/maven` | `.artifacts/windows/npm` | `.artifacts/windows/install/zlink-cpp/<version>` |
