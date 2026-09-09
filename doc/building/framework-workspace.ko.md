# Framework 작업 공간 구성

이 문서는 framework 저장소를 열었을 때 무엇이 기본으로 빌드되고, 무엇이 opt-in인지, 샘플과
bindings를 어떤 방식으로 참조하는지, C++는 어떤 preset으로 여는지를 한 곳에 모아 설명합니다.
결정은 2026-09-09에 확정된 것이며, 이후 구성이 바뀌면 이 문서를 먼저 고칩니다.

- 빌드 스크립트 위치와 배포 경로: [빌드·배포 파이프라인](./release-pipeline.ko.md)
- bindings를 package로 참조하는 이유와 원칙: [Framework와 Bindings 의존 경계](./framework-bindings-dependency-boundary.ko.md)
- 버전 번호 규칙: [버전 정책](./versioning.ko.md)

## 1. 기본 빌드에 들어가는 것과 빠지는 것

framework의 기본 빌드, CI, 배포는 **runtime library, unit test, `cross-language` e2e host**만
포함합니다. 다음 둘은 기본에서 빠지고 전용 run script로만 실행합니다.

| 대상 | 위치 | 실행 방법 |
|---|---|---|
| 언어별 시나리오 e2e | `framework/languages/<lang>/e2e/<name>/` | 각 `run_e2e.sh` / `run_e2e.ps1` |
| 7개 샘플(Bingo, DeliveryDispatch, GameQuest, ShoppingMall, SupportChat, TicTacToe, ZoneWorld) | `framework/languages/<lang>/samples/<name>/` | `samples/run_samples.sh` / `.ps1`, 샘플별 `run_sample.sh` / `.ps1`, Node는 `npm run sample` |

언어별로 "기본에서 빠진다"는 뜻은 다음과 같습니다.

| 언어 | 기본 빌드 단위 | 샘플·e2e가 빠지는 방식 |
|---|---|---|
| .NET | CI·배포는 `Zlink.Framework.ci.slnf`(16개 project)만 빌드 | 개발용 `Zlink.Framework.sln`은 IDE에서 열 수 있도록 샘플 project를 포함하지만 CI·배포는 slnf만 씁니다. 샘플은 각 `samples/<name>/<name>.sln`으로도 열립니다 |
| Node.js | `npm run verify:ci`(workspace `packages/*`) | 샘플은 workspace 밖의 독립 package입니다. `npm run test:samples`, `npm run lint:samples`, `npm run verify:samples`는 `verify:release`와 `framework-gate.sh`에서만 돌립니다 |
| Java/Kotlin | 루트 `settings.gradle.kts`의 module | 샘플은 `samples/` composite build입니다. 루트 빌드는 composite를 등록만 하고 task를 실행하지 않으며, CI·배포는 `-Pzlink.includeSamples=false`로 등록도 하지 않습니다 |
| C++ | `ZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF`, `ZLINK_FRAMEWORK_CPP_BUILD_E2E=OFF`, `ZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=ON`이 기본 | run script가 필요한 옵션을 `ON`으로 넘겨 같은 build tree에서 빌드합니다 |

## 2. 샘플의 두 모드

샘플 소스는 하나이고, 어디에서 열리는지에 따라 두 모드 중 하나로 동작합니다.

| 모드 | 조건 | framework 참조 | bindings 참조 |
|---|---|---|---|
| 개발자 모드 | 샘플이 저장소 안 `framework/languages/<lang>/samples/`에 있음 | 저장소의 framework 소스(ProjectReference, composite build, workspace link, `add_subdirectory`) | 언어별 기본 규칙(§3) |
| 사용자 모드 | 샘플 디렉터리를 저장소 밖으로 복사했거나 강제 옵션을 줌 | 공개 registry의 framework package(`0.10.0`) | 공개 registry의 bindings package(`0.17.6`) |

모드는 자동으로 판정합니다. 강제 옵션은 사용자 모드를 저장소 안에서 확인할 때만 씁니다.

| 언어 | 판정 기준 | 사용자 모드 강제 | 저장소 밖으로 복사 |
|---|---|---|---|
| .NET | `samples/Directory.Build.props`가 `../src/Zlink.Framework/Zlink.Framework.csproj` 존재 여부로 판정 | `-p:ZLinkSampleUseLocalSource=false` | `scripts/local-package/dotnet/prepare-sample.sh <Sample> <dest>` (`.ps1` 동일) |
| Node.js | `samples/scripts/prepare-sample-dependencies.mjs`가 상위 workspace(`@zlink-systems/node-framework-workspace`)를 찾으면 개발자 모드 | `ZLINK_NODE_SAMPLES_PACKAGE_MODE=1` | 샘플 디렉터리(`samples/<Name>.Ts`)를 그대로 복사한 뒤 `npm install` |
| Java/Kotlin | `samples/gradle/zlink-sample-dependencies.settings.gradle.kts`가 상위에서 framework `settings.gradle.kts`를 찾으면 개발자 모드 | `-Pzlink.samples.packageMode=true` (`-Pzlink.frameworkVersion`, `-Pzlink.bindingsVersion`으로 버전 지정) | `samples/` 디렉터리를 통째로 복사 |
| C++ | 샘플 `CMakeLists.txt`가 `../../CMakeLists.txt`와 `framework/include/zlink/framework.hpp`를 찾으면 개발자 모드 | `-DZLINK_FRAMEWORK_CPP_SAMPLES_PACKAGE_MODE=ON` | `samples/<Name>/` 디렉터리를 복사. `vcpkg.json`과 `conanfile.txt`가 `zlink`·`zlink-framework`를 요구 |

C++ 사용자 모드는 `zlink-framework` vcpkg port와 Conan recipe가 공개 자산에서 설치될 때 완성됩니다.
현재 port와 recipe는 저장소 overlay(`vcpkg/ports/`, `framework/languages/cpp/packaging/conan/`)에
있고, 공개 자산 재배포와 checksum 갱신 전에는 저장소 안 개발자 모드로만 샘플을 빌드합니다.

## 3. bindings 참조: package가 기본, source는 opt-in

framework는 bindings를 **배포된 package**로 참조합니다. bindings 소스를 고치면서 framework로 바로
확인해야 할 때만 소스 참조를 켭니다. 소스 참조는 샘플 사용자 모드와 함께 쓸 수 없습니다.

| 언어 | 기본(package) | 소스 참조 스위치 |
|---|---|---|
| .NET | NuGet `Zlink` (`ZLINK_LOCAL_PACKAGE_ROOT/nuget` 또는 `.artifacts/{wsl,windows}/nuget` local feed를 자동 추가) | `-p:ZLinkUseBindingsSource=true` |
| Node.js | npm `@zlink-systems/zlink` | `ZLINK_NODE_USE_BINDINGS_SOURCE=1` 또는 `npm run use:bindings-source` / `npm run use:bindings-package` |
| Java/Kotlin | Maven `systems.zlink:zlink` | `ZLINK_JAVA_BINDINGS_SOURCE=<bindings/java 경로>` |
| C++ | 설치된 `zlink_cpp` CMake package(`find_package(zlink_cpp <version> EXACT)`) | `-DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON` |

배포 전 버전의 bindings package는 [local package 가이드](../../scripts/local-package/README.ko.md)로
만들고 `ZLINK_LOCAL_PACKAGE_ROOT`로 가리킵니다.

## 4. C++ preset과 의존성 준비

C++ framework는 `framework/languages/cpp/CMakePresets.json`의 configure preset으로 엽니다.
preset 이름은 IDE·플랫폼 기준이고, 옵션 묶음은 `dev`·`ci` 둘입니다.

| preset | 용도 | generator | 비고 |
|---|---|---|---|
| `vs2022` | Visual Studio 2022 | Visual Studio 17 2022 | `.sln`은 CMake가 만듭니다. 별도 생성 script는 없습니다 |
| `windows-ninja` | Windows terminal, VS Code, CLion | Ninja | |
| `linux-ninja` | Linux, WSL | Ninja | |
| `macos-ninja` | Apple Silicon Mac | Ninja | `arm64-osx` triplet. Intel Mac은 Core부터 지원하지 않습니다 |
| `dev` | 저장소 개발 | Ninja | samples·e2e·tests·foundation tests·cross-language 모두 `ON` |
| `ci` | CI와 배포 검증 | Ninja | samples·e2e `OFF`, tests·cross-language `ON` |

모든 preset은 `VCPKG_ROOT`의 vcpkg toolchain과 저장소 overlay port(`vcpkg/ports`)를 씁니다. 준비는
bootstrap script 하나로 끝납니다.

```bash
source scripts/dev/bootstrap-cpp.sh        # vcpkg를 ~/.cache/zlink/vcpkg에 받고 VCPKG_ROOT·VCPKG_OVERLAY_PORTS 설정
cmake --preset linux-ninja -S framework/languages/cpp
cmake --build --preset linux-ninja
```

```powershell
. scripts/dev/bootstrap-cpp.ps1
cmake --preset vs2022 -S framework/languages/cpp
```

Conan을 쓰는 경우 `--conan`을 붙입니다. Conan은 독립 샘플(`conanfile.txt`)에서 `conan install`
뒤 `cmake --preset conan`으로 씁니다. 저장소 빌드의 기본은 vcpkg manifest입니다.

각 샘플 디렉터리에도 같은 이름의 preset이 있어 샘플 하나만 IDE에서 열 수 있습니다.

## 5. WSL과 Windows

명령은 WSL과 Windows에서 같습니다. 차이는 script 확장자(`.sh`/`.ps1`)와 preset 이름
(`linux-ninja`/`windows-ninja`·`vs2022`)뿐입니다. local package 산출물은 `.artifacts/wsl/`과
`.artifacts/windows/`로 나뉘어 서로 섞이지 않습니다.
