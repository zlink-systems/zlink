# Framework와 Bindings 의존 경계 정리

이 문서는 framework 언어 구현이 bindings 라이브러리를 어떤 방식으로 참조해야 하는지 정리한다.
목표는 framework가 bindings 소스 트리에 직접 묶이지 않게 하면서도, 로컬 개발과 CI 검증을
현실적으로 유지하는 것이다.

이 문서는 현재 공개 배포 상태를 보장하는 문서가 아니다. Maven, Conan, NuGet, npm 같은 외부
저장소에 실제로 배포되어 있는지는 별도로 확인해야 한다. 현재 저장소에는 배포를 위한 설정과
워크플로우가 있지만, 모든 저장소에 항상 배포되고 있다는 전제로 framework 빌드를 설계하면 안 된다.

## 1. 문제

framework가 bindings 소스를 직접 참조하면 다음 문제가 생긴다.

- bindings 코드를 수정하는 중에 framework 빌드와 테스트 결과가 같이 흔들린다.
- 실제 사용자가 설치하는 패키지와 로컬 framework가 참조하는 코드가 달라질 수 있다.
- 패키지에 포함되어야 할 native runtime, type declaration, CMake config, metadata 누락을
  framework 테스트가 잡지 못한다.
- framework가 bindings 내부 구조에 기대기 쉬워진다. framework는 bindings의 공개 API만 써야 한다.

그래서 기본 빌드 경로는 bindings 소스 참조가 아니라 bindings가 만든 패키지나 설치 산출물을
참조해야 한다. bindings 소스 참조는 bindings 자체를 디버깅할 때만 명시적으로 켜는 예외 경로로
남긴다.

## 2. 공통 원칙

### 2.1 기본값은 패키지 참조

framework의 기본 빌드는 언어별 패키지 매니저나 설치 경로를 통해 bindings를 참조한다.

| 언어 | 기본 참조 방식 | 로컬 검증 방식 |
|------|----------------|----------------|
| .NET | NuGet package | repo-local NuGet feed |
| Java/Kotlin | Maven artifact | repo-local Maven repository 또는 `mavenLocal()` |
| Node.js | npm package | `npm pack`으로 만든 tarball 또는 local registry |
| C++ | CMake package | install prefix, vcpkg overlay, Conan local cache |

framework 내부에 DLL, JAR, `.node`, `.so` 파일을 직접 복사해서 고정하는 방식은 권장하지 않는다.
복사는 처음에는 단순해 보이지만 플랫폼별 산출물, 버전, runtime asset, stale artifact 검증을
빌드 시스템 밖으로 밀어낸다. 패키지 매니저나 CMake package가 처리해야 할 일을 사람이 직접
관리하게 되는 구조다.

### 2.2 소스 참조는 opt-in

bindings를 수정하면서 framework로 바로 확인해야 할 때는 소스 참조가 필요할 수 있다. 이 경로는
명시 옵션으로만 켠다.

예:

```bash
# .NET 예시
dotnet test -p:ZLinkUseBindingsSource=true

# Java/Kotlin 예시
./gradlew test -Pzlink.useLocalBindings=true

# C++ 예시
cmake -S framework/languages/cpp -B build \
  -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON
```

옵션 이름은 실제 구현 시 언어별 기존 관례에 맞춰 정한다. 중요한 점은 기본값이 소스 참조가
아니어야 한다는 것이다.

### 2.3 CI는 패키지 경계를 검증해야 한다

CI에는 적어도 하나의 경로가 필요하다.

1. bindings를 먼저 빌드한다.
2. bindings 패키지나 설치 산출물을 만든다.
3. framework는 bindings 소스 트리를 참조하지 않고 그 산출물만으로 restore/build/test를 실행한다.

이 검증이 있어야 실제 배포 패키지에서 빠진 파일을 빨리 찾을 수 있다.

## 3. 현재 저장소에서 보이는 상태

이 절은 현재 코드 구조를 기준으로 정리한 것이다. 이후 빌드 파일이 바뀌면 다시 확인해야 한다.

### 3.1 .NET

현재 `.NET` framework project는 bindings `.csproj`를 직접 참조하는 경로가 있다.

대표 경로:

- `framework/languages/dotnet/src/Zlink.Framework/Zlink.Framework.csproj`
- `bindings/dotnet/src/Zlink/Zlink.csproj`

`bindings/dotnet/src/Zlink/Zlink.csproj`는 NuGet package metadata와 native runtime asset을
가진다. 따라서 `.NET`은 framework가 `ProjectReference`를 기본으로 쓰기보다 `PackageReference`를
기본으로 쓰는 편이 맞다.

권장 구조:

- 기본값: `PackageReference Include="Systems.Zlink"`
- 로컬 검증: `dotnet pack`으로 만든 `.nupkg`를 repo-local NuGet feed에 둔다.
- 예외 경로: `ZLinkUseBindingsSource=true`일 때만 bindings project를 직접 참조한다.

예시:

```bash
dotnet pack bindings/dotnet/src/Zlink/Zlink.csproj \
  -c Release \
  -o .artifacts/nuget

dotnet restore framework/languages/dotnet/Zlink.Framework.sln \
  --source .artifacts/nuget \
  --source https://api.nuget.org/v3/index.json
```

NuGet package는 managed assembly와 native runtime asset을 함께 다룰 수 있다. 그래서 framework
디렉터리에 DLL이나 native library를 직접 복사하는 방식보다 안전하다.

### 3.2 Java/Kotlin

현재 Java/Kotlin framework는 Maven 좌표와 Gradle composite build를 함께 갖고 있다.

현재 보이는 구조:

- framework build는 `systems.zlink:zlink` Maven 좌표를 사용한다.
- `framework/languages/java/settings.gradle.kts`에는 GitHub Packages repository 설정이 있다.
- 같은 파일에서 `zlink.useLocalBindings` 값이 true이면 `../../../bindings/java`를 `includeBuild`
  로 포함하고 `systems.zlink:zlink`를 local project로 대체한다.

이 구조는 방향은 맞지만 기본값을 조정할 필요가 있다. framework의 일반 빌드는 Maven artifact를
참조하고, bindings source composite build는 명시적으로 켜는 편이 좋다.

권장 구조:

- 기본값: `systems.zlink:zlink:<version>` Maven artifact 참조
- 로컬 검증: repo-local Maven repository 또는 `mavenLocal()`에 bindings artifact publish
- 예외 경로: `-Pzlink.useLocalBindings=true`일 때만 `includeBuild("../../../bindings/java")`

예시:

```bash
# bindings artifact를 로컬 Maven repository에 publish한다.
cd bindings/java
./gradlew publishToMavenLocal

# framework는 Maven artifact를 통해 빌드한다.
cd ../../framework/languages/java
./gradlew test -Pzlink.useLocalBindings=false
```

repo-local Maven repository를 쓰면 사용자 machine의 전역 `~/.m2` 상태에 덜 의존한다.

```bash
cd bindings/java
MAVEN_REPOSITORY_URL=file://$PWD/../../.artifacts/maven ./gradlew publish
```

실제 환경 변수나 Gradle property 이름은 구현 시 build script와 맞춰야 한다. 핵심은 framework가
bindings 소스 트리를 기본으로 include하지 않게 하는 것이다.

Kotlin framework는 Java framework와 같은 JVM artifact를 사용한다. Kotlin만을 위한 별도 bindings
복사 경로를 만들 필요는 없다.

### 3.3 Node.js

현재 Node framework에는 bindings package를 직접 파일 경로로 참조하는 부분이 있다.

대표 구조:

- `framework/languages/node/packages/framework/package.json`의
  `@zlink-systems/zlink: file:../../../../../bindings/node`
- 여러 `tsconfig.json`의 `bindings/node/dist/index.d.ts` 직접 경로
- build script의 `../../../bindings/node/node_modules/typescript/bin/tsc` 직접 사용

이 방식은 로컬 개발에는 빠르지만 package 경계를 검증하지 못한다. Node는 `npm pack`으로 만든
tarball을 로컬 package로 설치하는 방식이 더 낫다.

권장 구조:

- 기본값: `@zlink-systems/zlink` version dependency
- 로컬 검증: `npm pack`으로 만든 `.tgz` 설치
- 예외 경로: bindings 개발 중에만 `file:` 또는 workspace link 사용

예시:

```bash
cd bindings/node
npm ci
npm run build
npm run rebuild-native
npm pack --pack-destination ../../.artifacts/npm

cd ../../framework/languages/node
npm install ../../../.artifacts/npm/zlink-systems-zlink-*.tgz
npm test
```

`npm link`는 machine-local 상태에 크게 의존하므로 재현 가능한 검증 경로로 쓰지 않는다. 필요하면
수동 디버깅용으로만 사용한다.

Node framework package도 나중에 배포할 계획이면 framework 자체도 `npm pack` 후 sample/e2e가
packed artifact를 설치해 검증하는 경로를 두는 것이 좋다. 이렇게 해야 `files`, `exports`,
`types`, native prebuild 포함 누락을 잡을 수 있다.

### 3.4 C++

현재 C++ framework는 `framework/languages/cpp/CMakeLists.txt`에서 bindings C++ project를
`add_subdirectory`로 직접 포함하는 경로가 있다. 동시에 framework install consumer test는
`find_package(zlink_framework_cpp CONFIG REQUIRED)`와 `find_package(zlink_stream_connector_cpp CONFIG REQUIRED)`
로 설치된 package config를 소비하는 흐름을 검증한다.

C++은 단순 binary copy보다 CMake package install prefix를 쓰는 편이 좋다. header, library,
imported target, transitive dependency, runtime path를 CMake가 함께 관리할 수 있기 때문이다.

권장 구조:

- 기본값: `find_package(zlink_cpp CONFIG REQUIRED)`로 bindings C++ package 참조
- 로컬 검증: bindings C++를 install prefix에 설치하고 `CMAKE_PREFIX_PATH`로 framework configure
- 예외 경로: C++ bindings 개발 중에만 `add_subdirectory` source mode 사용

예시:

```bash
cmake -S bindings/cpp -B .artifacts/build/bindings-cpp \
  -DCMAKE_INSTALL_PREFIX=$PWD/.artifacts/install/zlink-cpp \
  -DZLINK_CPP_BUILD_TESTS=OFF \
  -DZLINK_CPP_BUILD_SAMPLES=OFF

cmake --build .artifacts/build/bindings-cpp
cmake --install .artifacts/build/bindings-cpp

cmake -S framework/languages/cpp -B .artifacts/build/framework-cpp \
  -DCMAKE_PREFIX_PATH=$PWD/.artifacts/install/zlink-cpp \
  -DZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=OFF

cmake --build .artifacts/build/framework-cpp
ctest --test-dir .artifacts/build/framework-cpp --output-on-failure
```

장기적으로는 vcpkg registry나 Conan package를 쓸 수 있다. 다만 현재 저장소에 Conan workflow와
vcpkg overlay가 있다고 해서 public ConanCenter 또는 공식 vcpkg ports에 이미 배포되어 있다고
가정하면 안 된다. 공식 저장소 배포 여부는 release 시점에 별도로 확인한다.

## 4. 외부 저장소와 로컬 저장소의 구분

이 문서에서 말하는 package reference는 꼭 public registry를 뜻하지 않는다.

| 구분 | 목적 | 예 |
|------|------|----|
| public registry | 사용자 배포 | NuGet.org, Maven Central, npm, ConanCenter |
| private registry | 조직 내부 배포 | GitHub Packages, 사내 NuGet/Maven/npm/Conan registry |
| repo-local feed | 로컬/CI 검증 | `.artifacts/nuget`, `.artifacts/maven`, `.artifacts/npm`, install prefix |
| source mode | bindings 개발 | ProjectReference, Gradle includeBuild, npm file link, CMake add_subdirectory |

정식 배포 전에는 repo-local feed와 private registry만으로도 충분하다. 중요한 것은 framework가
bindings 소스가 아니라 "패키지로 만들어진 결과"를 소비하는 검증 경로를 갖는 것이다.

## 5. WSL과 Windows 로컬 패키지 분리

로컬 배포 패키지 검증은 WSL과 Windows를 분리해서 생각해야 한다. 둘은 파일 경로 규칙뿐 아니라
native runtime 파일 형식도 다르다.

| 환경 | 대표 native 파일 | 주의점 |
|------|------------------|--------|
| WSL/Linux | `libzlink.so` | Linux용 package와 install prefix를 사용한다. |
| Windows | `zlink.dll` | Windows용 package와 install prefix를 사용한다. |

한 환경에서 만든 native 산출물을 다른 환경의 framework 검증에 그대로 쓰면 안 된다. 예를 들어 WSL에서
만든 `.so`만 들어 있는 package로 Windows `.NET` 또는 Node framework를 검증하면 실제 사용자 설치
조건을 확인한 것이 아니다.

### 5.1 권장 디렉터리 구조

local artifact는 환경별로 나눈다.

```text
.artifacts/
  wsl/
    nuget/
    maven/
    npm/
    install/
      zlink-cpp/
  windows/
    nuget/
    maven/
    npm/
    install/
      zlink-cpp/
```

이렇게 나누면 같은 version을 검증하더라도 WSL과 Windows 산출물이 섞이지 않는다.

### 5.2 WSL에서 실행하는 검증

WSL에서는 Linux native runtime을 포함한 package를 만든 뒤 framework를 검증한다.

예:

```bash
ZLINK_LOCAL_PACKAGE_ROOT=.artifacts/wsl

dotnet pack bindings/dotnet/src/Zlink/Zlink.csproj \
  -c Release \
  -o "$ZLINK_LOCAL_PACKAGE_ROOT/nuget"

cd bindings/node
npm ci
npm run build
npm run rebuild-native
npm pack --pack-destination ../../.artifacts/wsl/npm
```

C++은 WSL install prefix를 따로 둔다.

```bash
cmake -S bindings/cpp -B .artifacts/wsl/build/bindings-cpp \
  -DCMAKE_INSTALL_PREFIX=$PWD/.artifacts/wsl/install/zlink-cpp

cmake --build .artifacts/wsl/build/bindings-cpp
cmake --install .artifacts/wsl/build/bindings-cpp
```

### 5.3 Windows에서 실행하는 검증

Windows에서는 PowerShell 기준 스크립트를 별도로 둔다. 이 경로는 Windows native runtime을 포함한
package를 만들고 Windows framework 빌드를 검증한다.

예:

```powershell
$env:ZLINK_LOCAL_PACKAGE_ROOT = ".artifacts/windows"

dotnet pack bindings/dotnet/src/Zlink/Zlink.csproj `
  -c Release `
  -o "$env:ZLINK_LOCAL_PACKAGE_ROOT/nuget"

Push-Location bindings/node
npm ci
npm run build
npm run rebuild-native
npm pack --pack-destination ../../.artifacts/windows/npm
Pop-Location
```

C++은 Windows generator와 toolchain에 맞춰 별도 build directory를 사용한다.

```powershell
cmake -S bindings/cpp -B .artifacts/windows/build/bindings-cpp `
  -DCMAKE_INSTALL_PREFIX="$PWD/.artifacts/windows/install/zlink-cpp"

cmake --build .artifacts/windows/build/bindings-cpp --config Release
cmake --install .artifacts/windows/build/bindings-cpp --config Release
```

### 5.4 스크립트 구성

스크립트도 환경별 entrypoint를 두는 편이 안전하다.

```text
scripts/local-package/build-wsl.sh
scripts/local-package/native/sync-local-core-libs.sh
scripts/local-package/<lang>/build-wsl.sh
```

공통 정책은 같지만 명령, 경로 구분자, native runtime, CMake generator가 다르기 때문이다.
현재 local package 생성 스크립트의 사용법은
[`scripts/local-package/README.ko.md`](../../scripts/local-package/README.ko.md)에 둔다.

로컬 배포 스크립트는 `scripts/local-package/` 한 곳에만 둔다. `bindings/<lang>/` 아래에는
NuGet, Maven, npm, CMake install 같은 package metadata와 build rule만 유지한다. 실행 스크립트를
bindings 디렉터리마다 중복해서 두면 WSL/Windows 산출물 위치와 검증 정책이 갈라지기 쉽다.

core release 산출물이나 로컬 core 빌드 결과를 bindings workspace로 복사하는 스크립트도
`scripts/local-package/native/`에서 관리한다. 기존에 `bindings/` 또는 `core/tools/` 아래에 있던
동기화 스크립트는 유지하지 않고, 자동화와 문서는 새 경로를 직접 호출한다.

`.NET` 바인딩은 `bindings/dotnet/native/<rid>/`를 native library 기준 위치로 사용한다. NuGet
package를 만들 때만 이 파일들을 `runtimes/<rid>/native/` 구조로 넣는다. 따라서
`bindings/dotnet/runtimes/`를 별도 소스 입력으로 갱신하지 않는다.

## 6. Git 저장소에 binary를 넣는 방식

Git에 built binary를 커밋해서 framework가 그 파일을 참조하게 하는 방식은 기본 전략으로 쓰지
않는다.

문제점:

- 플랫폼별 파일 수가 늘어난다.
- 누가 언제 만든 binary인지 추적하기 어렵다.
- source 변경 뒤 binary를 갱신하지 않아도 git diff만으로는 알기 어렵다.
- package metadata 검증이 빠진다.

예외적으로 offline 개발을 위해 vendored package cache를 둘 수는 있다. 이 경우에도 그냥 DLL이나
JAR를 복사하지 말고, NuGet package, Maven artifact, npm tarball, CMake install archive처럼
패키지 단위를 저장해야 한다. 또한 checksum과 생성 절차를 문서화해야 한다.

## 7. 권장 전환 순서

### 7.1 1단계: 현재 직접 참조 목록 확인

각 언어에서 bindings 소스 직접 참조를 모두 찾는다.

```bash
rg -n "bindings/(dotnet|java|node|cpp)|ProjectReference|includeBuild|file:|add_subdirectory" \
  framework/languages
```

이 목록을 바탕으로 기본 빌드 경로와 source mode 경로를 나눈다.

### 7.2 2단계: bindings package 생성 명령 고정

각 bindings가 로컬 package를 만드는 명령을 하나씩 둔다.

| 언어 | 산출물 |
|------|--------|
| .NET | `.nupkg` |
| Java/Kotlin | Maven repository layout |
| Node.js | `.tgz` |
| C++ | CMake install prefix 또는 package archive |

명령은 CI와 로컬에서 같은 방식으로 동작해야 한다.

### 7.3 3단계: framework 기본 빌드를 package mode로 전환

framework build file에서 source reference를 기본값에서 제거한다. 대신 package feed 위치나
version을 받는 옵션을 둔다.

예:

```bash
ZLINK_BINDINGS_VERSION=8.6.3
ZLINK_LOCAL_PACKAGE_ROOT=.artifacts
```

옵션 이름은 언어별 도구에 맞게 정하되, 의미는 같아야 한다.

### 7.4 4단계: source mode를 명시 옵션으로 유지

bindings 개발자에게는 source mode가 필요하다. 다만 이 경로는 명시적으로 켠 경우에만 동작해야
한다.

예:

```bash
ZLINK_USE_BINDINGS_SOURCE=true
```

source mode에서도 framework가 bindings의 public API만 사용한다는 규칙은 유지한다. internal 또는
private member를 reflection이나 friend 설정으로 우회하지 않는다.

### 7.5 5단계: package boundary CI 추가

CI에는 다음 이름의 검증 단계를 둔다.

```text
build-bindings-package
build-framework-from-bindings-package
run-framework-tests
```

이 단계에서는 source mode를 꺼야 한다. source mode가 켜져 있으면 package 누락을 잡을 수 없기
때문이다.

## 8. 언어별 최종 권장안

| 언어 | 지금 우선 적용할 방향 | 장기 방향 |
|------|------------------------|-----------|
| .NET | local NuGet feed + `PackageReference` 기본화 | NuGet.org 또는 private NuGet |
| Java/Kotlin | `zlink.useLocalBindings=false` 기본화, local Maven feed 검증 | GitHub Packages 또는 Maven Central |
| Node.js | `file:` 기본 참조 제거, packed tarball 설치 검증 | npm registry 또는 private npm registry |
| C++ | `add_subdirectory` 기본 제거, CMake install package 소비 | vcpkg registry 또는 Conan package |

## 9. 완료 기준

전환이 완료되었다고 보려면 다음 조건을 만족해야 한다.

- framework 기본 빌드가 bindings 소스 트리를 참조하지 않는다.
- bindings package를 새로 만든 뒤 framework가 그 package만으로 빌드된다.
- source mode는 명시 옵션으로만 켜진다.
- sample과 e2e도 package mode에서 최소 한 번 검증된다.
- WSL과 Windows 검증 산출물이 섞이지 않는다.
- 패키지에 native runtime, header, type declaration, metadata가 빠지면 CI가 실패한다.
- 문서와 build script가 "외부 저장소에 이미 배포되어 있다"는 전제를 두지 않는다.
