# Local package

이 디렉터리는 외부 registry에 publish하지 않고 Core release와 first-party
binding을 같은 `0.11.0` 기준으로 package하는 경로다. 기본 동작은 GitHub의
`core/v0.11.0` release asset을 다운로드하고 checksum과 provenance를 확인한
뒤 binding이 사용할 Core prefix를 만드는 것이다. 기본 출력은
`.artifacts/wsl/` 아래에 생성된다.

## 전체 빌드

```bash
scripts/local-package/build-wsl.sh
```

위 명령은 Core source를 별도로 build하지 않고 release Core를 준비한 뒤 C,
C++, .NET, Go, Java, Node.js, Python, Rust binding을 차례로 package한다.

특정 binding만 package하려면 언어 이름을 넘긴다.

```bash
scripts/local-package/build-wsl.sh dotnet java node
```

진행 중인 Core source 변경을 함께 검증해야 할 때만 local source mode를
명시한다.

```bash
scripts/local-package/build-wsl.sh --core-source local
```

local Core package를 직접 만들거나 별도 출력 위치를 사용할 때는 다음처럼
실행한다.

```bash
scripts/local-package/core/build-wsl.sh \
  --output-root /absolute/path/.artifacts/wsl
```

Core local package는 다음 구조를 사용한다.

```text
.artifacts/wsl/install/zlink-core/0.11.0/
  include/
  lib/libzlink.so
  lib/libzlink.so.0
  lib/libzlink.so.0.11.0
  share/zlink/core-package-provenance.json
```

release Core prefix는 기본적으로 `~/.cache/zlink/core/0.11.0/linux-x64/`에
cache된다. 이미 같은 version과 platform의 provenance가 있으면 다운로드와
Core build를 반복하지 않는다. 다른 위치를 사용하려면 다음처럼 지정한다.

```bash
bash scripts/local-package/core/fetch-release.sh \
  --version 0.11.0 \
  --platform linux-x64 \
  --cache-dir /absolute/path/zlink-core-cache
```

`0.11.0`은 release/package version이다. native runtime의 SONAME도 같은
release line에 맞춰 `libzlink.so.0`으로 생성한다. 외부 dependency의 버전은
이 정책의 대상이 아니다.

## binding별 출력

- C: `.artifacts/wsl/c/zlink-c-0.11.0.tar.gz`
- C++: `.artifacts/wsl/install/zlink-cpp/0.11.0/`
- .NET: `.artifacts/wsl/nuget/Systems.Zlink.0.11.0.nupkg`
- Go: `.artifacts/wsl/go/zlink-go-0.11.0.tar.gz`
- Java: `.artifacts/wsl/maven/systems/zlink/zlink/0.11.0/`
- Node.js: `.artifacts/wsl/npm/zlink-systems-zlink-0.11.0.tgz`
- Python: `.artifacts/wsl/python/zlink-0.11.0-*.whl` 및 source archive
- Rust: `.artifacts/wsl/rust/zlink-0.11.0.crate`

Go의 public module path는 `zlink.systems/zlink`이며, release version과
import path를 분리한다. 모든 binding package는 Core provenance에 기록된
`0.11.0` runtime과 public header를 사용한다.

## Windows native 검증

Windows 작업에서도 binding은 Core source를 먼저 build하지 않고 release prefix를
사용한다. 다음 명령은 Windows x64 Core release를 다운로드하고 검증한다.

```powershell
$prefix = powershell -ExecutionPolicy Bypass -File scripts/local-package/core/fetch-release.ps1
```

기본 prefix는 `%LOCALAPPDATA%\zlink\core\0.11.0\windows-x64\`이다. 진행 중인
Windows Core 변경이 필요한 경우에는 기존 `core/build/windows-x64/install/`을
local source fallback 입력으로 사용한다. WSL 출력과 Windows 출력을 서로 바꾸어
사용하지 않는다.

Windows의 local source fallback build 조건은 `.github/workflows/build.yml`의 Windows x64 job과 맞춘다.
즉 Visual Studio 17 2022 x64 generator에서 `Release`, `BUILD_SHARED=ON`,
`BUILD_STATIC=ON`, `BUILD_TESTS=OFF`, C++17을 사용하고, `ENABLE_LTO`는 기본값을
유지한다. 이 조건의 표준 runtime은 MSVC dynamic CRT(`/MD`)이며, 결과는
`core/build/windows-x64/install/`에 설치한다.

Java 22 FFM에서 JDK가 먼저 로드한 `msvcp140.dll`과 `/MD` Core의 C++ runtime이
충돌하는 환경에서는 Java 검증용 `/MT` Core를 별도 build directory로 만든다. 이
variant를 공통 CI runtime이나 다른 binding의 staged runtime과 섞지 않으며, Java
계획 문서에서 별도 증적으로 기록한다.

Windows package 입력과 결과는 다음 경로를 사용한다.

- .NET: `ZLinkWindowsX64NativeRoot=<release-prefix>/bin`, 결과는
 `.artifacts/windows/dotnet/package/`
- C++: `<release-prefix>/`를 `ZLINK_CPP_CORE_PACKAGE_PREFIX`로 지정하고 CMake install 결과
 `.artifacts/windows/cpp/package/`
- Go: release prefix의 `bin/` runtime을 `bindings/go/native/windows-x86_64/`에 배치하고, 결과는 `.artifacts/windows/go/package/`
- Java: release prefix의 `bin/zlink.dll`을 사용하는 version-only consumer
- Node.js: release prefix의 `bin/zlink.dll`을 `bindings/node/prebuilds/win32-x64/`에 배치하고, 결과는 `.artifacts/windows/node/package/`
- Python: wheel의 `native/windows-x86_64/zlink.dll`에 release prefix runtime을 배치하고, 결과는 `.artifacts/windows/python/wheel/`
- Rust: crate의 `native/windows-x86_64/`에 release prefix runtime을 배치하고, 결과는 `.artifacts/windows/rust/`

Windows native package 생성 절차를 통합할 때는 이 경로와 언어별 version pinning을 함께
갱신한다. 현재 Windows 성능 실행 결과의 상태와 실패 원인은
`doc/perf/perf/core-0.10.0/` 아래의 개별 measurement sheet와 `log/`가 소유한다.

## Core runtime 동기화

`native/sync-local-core-libs.sh`는 `ZLINK_CORE_PACKAGE_PREFIX`가 가리키는
검증된 Core prefix의 `0.11.0` runtime과 public header를 binding 작업
디렉터리에 복사한다. 이 환경 변수가 없을 때만 `core/build/lib`와
`core/include`를 local source fallback으로 사용한다.

```bash
scripts/local-package/native/sync-local-core-libs.sh
```

이 파일들은 local package build 입력이다. release package를 만들 때는
script가 생성한 native 파일을 별도로 commit하지 않는다.
