# Local package

이 디렉터리는 외부 registry에 publish하지 않고 Core release와 first-party
binding을 package하는 경로다. Core version은 root `VERSION`, binding package
version은 `bindings/<language>/VERSION`, Framework package version은
`framework/languages/<language>/VERSION`이 각각 소유한다. 기본 동작은 GitHub의
`core/v<VERSION>` release asset을 다운로드하고 checksum과 provenance를 확인한
뒤 binding이 사용할 Core prefix를 만드는 것이다. 기본 출력은
`.artifacts/wsl/` 아래에 생성된다.

## 버전 동기화

root `VERSION`은 Core release·public header·native payload version의 유일한
원본이다. 각 binding과 Framework의 `VERSION`은 해당 언어 package release
version의 유일한 원본이다. package manager manifest와 Framework binding
dependency pin은 같은 언어의 binding `VERSION`을, Core prefix·provenance·versioned
runtime은 root `VERSION`을, Framework manifest와 sample dependency pin은 같은
언어의 Framework `VERSION`을 따른다. C binding은 Core와 함께 배포하므로 별도
package version 없이 root `VERSION`을 사용한다. 다음 공식 진입점으로 동기화하고
검증한다.

Core, binding package 또는 Framework package release version을 변경할 때는 해당
소유 `VERSION` 파일만 수정한 뒤 `--sync-versions`를 실행한다. 언어별 manifest,
Framework dependency, sample pin과 sample runner의 local Core package 경로를 직접
찾아서 수정하지 않는다. 동기화 뒤에는 `--verify-versions`로 누락된 pin이 없는지
확인하고 local package를 생성한다.

```bash
scripts/local-package/build-wsl.sh --sync-versions
scripts/local-package/build-wsl.sh --verify-versions
```

일반 local-package build와 cache hit는 버전을 검사하고, 불일치하면 종료한다.
버전 파일을 변경한 뒤에는 위의 `--sync-versions`를 명시적으로 실행한다.
Framework package 자체 version과 언어별 sample pin도 같은 동기화 대상이다.

## 전체 빌드

```bash
scripts/local-package/build-wsl.sh
```

위 명령은 Core source를 별도로 build하지 않고 root `VERSION`의 release Core를
준비한 뒤, C는 Core version으로, 나머지는 각 `bindings/<language>/VERSION`으로
C, C++, .NET, Go, Java, Node.js, Python, Rust binding을 차례로 package한다.

`bindings/`와 `scripts/local-package/`가 깨끗하면 입력 hash에 해당하는 공유
binding package를 재사용한다. Cache miss에서는 8개 binding을 모두 빌드한다.
`.artifacts/wsl/`은 worktree별 디렉터리이며 binding package 파일만 공유 cache로
연결한다. Framework package와 빌드 디렉터리는 worktree별로 유지한다.

위 입력 경로에 staged·unstaged·untracked 변경이 있거나 기본 Release와 다른
빌드 설정·compiler flag를 사용하면 `.artifacts/wsl-private/`에서 빌드한다.
이때 특정 binding만 package하려면 언어 이름을 넘긴다.

```bash
scripts/local-package/build-wsl.sh dotnet java node
```

Cache key·도구 버전 조회와 cache 정리는 다음 명령을 사용한다.
공유 정책은 [개발 흐름 §4.1](../../doc/principal/dev/development-workflow.ko.md#41-로컬-패키지-공유-캐시-content-addressed)이 소유한다.

```bash
scripts/local-package/build-wsl.sh --cache-key
scripts/local-package/cache-prune.sh --keep 5 --dry-run
scripts/local-package/cache-prune.sh --keep 5
```

**Core release가 선행 조건이다 — 우회 경로는 없다(2026-08-28 확정).** Core source
변경을 검증할 때도 로컬 빌드로 대신하지 않고, 먼저 release를 만든 뒤 이 경로로
패키징한다. 절차는 다음으로 고정한다.

```bash
# ① root VERSION 확정 후 동기화·검증
scripts/local-package/build-wsl.sh --sync-versions
scripts/local-package/build-wsl.sh --verify-versions

# ② release 커밋에 태그를 만들어 푸시한다
git tag core/v<VERSION> <release-commit> && git push origin core/v<VERSION>

# ③ 태그 ref로 빌드 워크플로를 dispatch한다 — build.yml은 태그 push로는 돌지 않는다
GH_REPO=zlink-systems/zlink gh workflow run build.yml --ref core/v<VERSION> -f libzlink_version=<VERSION>

# ④ release asset 생성을 확인한다
GH_REPO=zlink-systems/zlink gh release view core/v<VERSION>

# ⑤ local package를 생성한다 (release 다운로드 + checksum·provenance 검증)
scripts/local-package/build-wsl.sh cpp dotnet java node
```

release가 아직 없으면 ⑤가 404로 실패하는 것이 정상이다 — 그때는 ②~④를 먼저
끝낸다. 이전에 있던 `--core-source local`·`--core-prefix` 우회와
`core/build-wsl.sh` 로컬 core 빌더는 제거했다.

Core local package는 다음 구조를 사용한다.

```text
.artifacts/wsl/install/zlink-core/<VERSION>/
  include/
  lib/libzlink.so
  lib/libzlink.so.0
  lib/libzlink.so.<VERSION>
  share/zlink/core-package-provenance.json
```

release Core prefix는 기본적으로 `~/.cache/zlink/core/<VERSION>/linux-x64/`에
cache된다. 이미 같은 version과 platform의 provenance가 있으면 다운로드와
Core build를 반복하지 않는다. 다른 위치를 사용하려면 다음처럼 지정한다.

```bash
bash scripts/local-package/core/fetch-release.sh \
  --version <VERSION> \
  --platform linux-x64 \
  --cache-dir /absolute/path/zlink-core-cache
```

`<VERSION>`은 release/package version이다. native runtime의 SONAME도 같은
release line에 맞춰 `libzlink.so.0`으로 생성한다. 외부 dependency의 버전은
이 정책의 대상이 아니다.

## binding별 출력

- C: `.artifacts/wsl/c/zlink-c-<CORE_VERSION>.tar.gz`
- C++: `.artifacts/wsl/install/zlink-cpp/<CPP_BINDING_VERSION>/`
- .NET: `.artifacts/wsl/nuget/Zlink.<DOTNET_BINDING_VERSION>.nupkg`
- Go: `.artifacts/wsl/go/zlink-go-<GO_BINDING_VERSION>.tar.gz`
- Java: `.artifacts/wsl/maven/systems/zlink/zlink/<JAVA_BINDING_VERSION>/`
- Node.js: `.artifacts/wsl/npm/zlink-systems-zlink-<NODE_BINDING_VERSION>.tgz`
- Python: `.artifacts/wsl/python/zlink-<PYTHON_BINDING_VERSION>-*.whl` 및 source archive
- Rust: `.artifacts/wsl/rust/zlink-<RUST_BINDING_VERSION>.crate`

Go의 public module path는 `zlink.systems/zlink`이며, release version과
import path를 분리한다. 모든 binding package는 Core provenance에 기록된
`VERSION` runtime과 public header를 사용한다.

## Windows native 검증

Windows 작업에서도 기본 입력은 Core source build가 아니라 검증된 release prefix다.
fetch script는 archive checksum과 provenance를 검증하고 `zlink.dll`의 PE machine이
요청한 platform과 일치하는지도 확인한다.

```powershell
$x64Prefix = powershell -ExecutionPolicy Bypass -File scripts/local-package/core/fetch-release.ps1 `
  -Platform windows-x64
$arm64Prefix = powershell -ExecutionPolicy Bypass -File scripts/local-package/core/fetch-release.ps1 `
  -Platform windows-arm64

# C++/.NET/Java/Node binding 전체 또는 언어별 package
scripts/local-package/build-windows.ps1 -SyncVersions
scripts/local-package/build-windows.ps1 -VerifyVersions
scripts/local-package/build-windows.ps1 -CorePrefix $x64Prefix -Architecture x64
scripts/local-package/build-windows.ps1 -CorePrefix $arm64Prefix -Architecture arm64
scripts/local-package/cpp/build-windows.ps1 -CorePrefix $x64Prefix
scripts/local-package/dotnet/build-windows.ps1 -CorePrefix $x64Prefix
scripts/local-package/java/build-windows.ps1 -CorePrefix $x64Prefix
scripts/local-package/node/build-windows.ps1 -CorePrefix $x64Prefix

# Framework가 소비하는 .NET/Java/Node HTTP client local package
scripts/local-package/http-client/build-windows.ps1
```

release cache prefix는
`%LOCALAPPDATA%\zlink\core\<VERSION>\windows-<architecture>\`이다. provenance의
`platform`과 runtime PE machine이 다르면 package 생성 전에 실패한다. `-Architecture`를
생략하면 provenance에서 target을 선택하고, 지정하면 입력과 일치하는지 추가로 검증한다.
WSL 출력과 Windows 출력을 서로 바꾸어 사용하지 않는다.

진행 중인 Core 변경을 binding에서 디버깅할 때만 local rebuild 진입점을 사용한다.

```powershell
scripts/gate/rebuild-dev.ps1 -Architecture x64 -Language cpp,dotnet,java,node
scripts/gate/rebuild-dev.ps1 -Architecture arm64 -Language cpp,dotnet,java,node
```

이 script는 `.github/workflows/build.yml`의 Windows 조건과 같은 Visual Studio 2022
generator, `BUILD_SHARED=ON`, `BUILD_STATIC=ON`, `BUILD_TESTS=OFF`, C++17을 사용한다.
build directory는 `core/build/windows-<architecture>/`, 검증된 prefix는 architecture별
artifact root의 `install/zlink-core/<VERSION>/`이다. x64 artifact root는 기존 호환 경로인
`.artifacts/windows/`, ARM64는 `.artifacts/windows-arm64/`를 사용한다. ARM64 local
rebuild에는 ARM64용 OpenSSL 개발 prefix가 필요하다.

Java 22 FFM에서 JDK가 먼저 로드한 `msvcp140.dll`과 `/MD` Core의 C++ runtime이
충돌하는 환경에서는 Java 검증용 `/MT` Core를 별도 build directory로 만든다. 이
variant를 공통 CI runtime이나 다른 binding의 staged runtime과 섞지 않으며, Java
계획 문서에서 별도 증적으로 기록한다.

Windows package target과 결과는 Core platform에서 함께 결정한다.

- C++: generator platform `x64` 또는 `ARM64`, 결과는
  `<artifact-root>/install/zlink-cpp/<CPP_BINDING_VERSION>/`
- .NET: native RID `win-x64` 또는 `win-arm64`, 결과는
  `<artifact-root>/nuget/Zlink.<DOTNET_BINDING_VERSION>.nupkg`
- Java: resource `native/windows-x86_64/` 또는 `native/windows-aarch64/`, 결과는
  `<artifact-root>/maven/systems/zlink/zlink/<JAVA_BINDING_VERSION>/`
- Node.js: prebuild `win32-x64/` 또는 `win32-arm64/`, 결과는
  `<artifact-root>/npm/zlink-systems-zlink-<NODE_BINDING_VERSION>.tgz`

각 package는 반대 architecture의 payload가 섞이면 실패한다. C++ local install에는
Core platform과 library hash를 기록한 `share/zlink/zlink-cpp-package-provenance.json`도
생성한다. Windows native package 생성 절차를 바꿀 때는 이 target mapping과 언어별
version pinning을 함께 갱신한다.

## Core runtime 동기화

`native/sync-local-core-libs.sh`는 `ZLINK_CORE_PACKAGE_PREFIX`가 가리키는
검증된 Core prefix의 `<VERSION>` runtime과 public header를 binding 작업
디렉터리에 복사한다. 이 환경 변수가 없을 때만 `core/build/lib`와
`core/include`를 local source fallback으로 사용한다.

```bash
scripts/local-package/native/sync-local-core-libs.sh
```

이 파일들은 local package build 입력이다. release package를 만들 때는
script가 생성한 native 파일을 별도로 commit하지 않는다.
