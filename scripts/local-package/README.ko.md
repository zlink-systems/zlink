# Local package

이 디렉터리는 외부 registry에 publish하지 않고 Core와 first-party binding을
같은 `0.10.1` source 기준으로 빌드하는 경로다. 기본 출력은
`.artifacts/wsl/` 아래에 생성된다.

## 전체 빌드

```bash
scripts/local-package/build-wsl.sh
```

위 명령은 Core를 먼저 빌드한 뒤 C, C++, .NET, Go, Java, Node.js, Python,
Rust binding을 차례로 package한다.

특정 binding만 빌드하려면 언어 이름을 넘긴다.

```bash
scripts/local-package/build-wsl.sh dotnet java node
```

Core만 다시 만들거나 별도 출력 위치를 사용할 때는 다음처럼 실행한다.

```bash
scripts/local-package/core/build-wsl.sh \
  --output-root /absolute/path/.artifacts/wsl
```

Core local package는 다음 구조를 사용한다.

```text
.artifacts/wsl/install/zlink-core/0.10.1/
  include/
  lib/libzlink.so
  lib/libzlink.so.0
  lib/libzlink.so.0.10.1
  share/zlink/core-package-provenance.json
```

`0.10.1`은 release/package version이다. native runtime의 SONAME도 같은
release line에 맞춰 `libzlink.so.0`으로 생성한다. 외부 dependency의 버전은
이 정책의 대상이 아니다.

## binding별 출력

- C: `.artifacts/wsl/c/zlink-c-0.10.1.tar.gz`
- C++: `.artifacts/wsl/install/zlink-cpp/0.10.1/`
- .NET: `.artifacts/wsl/nuget/Systems.Zlink.0.10.1.nupkg`
- Go: `.artifacts/wsl/go/zlink-go-0.10.1.tar.gz`
- Java: `.artifacts/wsl/maven/systems/zlink/zlink/0.10.1/`
- Node.js: `.artifacts/wsl/npm/zlink-systems-zlink-0.10.1.tgz`
- Python: `.artifacts/wsl/python/zlink-0.10.1-*.whl` 및 source archive
- Rust: `.artifacts/wsl/rust/zlink-0.10.1.crate`

Go의 public module path는 `zlink.systems/zlink`이며, release version과
import path를 분리한다. 모든 binding package는 Core provenance에 기록된
`0.10.1` runtime과 public header를 사용한다.

## Windows native 검증

`build-wsl.sh`와 그 아래의 `build-*.sh`는 기존 Linux local package 생성 경로이며
Windows PowerShell에서 실행하는 통합 build script가 아니다. Windows native 성능 검증은
`core/build/windows-x64/install/`에 설치한 MSVC Core runtime을 기준으로 각 binding의
Windows package 입력과 public consumer를 확인한다. Windows 작업에서 WSL 출력과
Windows 출력을 서로 바꾸어 사용하지 않는다.

공통 Windows Core build 조건은 `.github/workflows/build.yml`의 Windows x64 job과 맞춘다.
즉 Visual Studio 17 2022 x64 generator에서 `Release`, `BUILD_SHARED=ON`,
`BUILD_STATIC=ON`, `BUILD_TESTS=OFF`, C++17을 사용하고, `ENABLE_LTO`는 기본값을
유지한다. 이 조건의 표준 runtime은 MSVC dynamic CRT(`/MD`)이며, 결과는
`core/build/windows-x64/install/`에 설치한다.

Java 22 FFM에서 JDK가 먼저 로드한 `msvcp140.dll`과 `/MD` Core의 C++ runtime이
충돌하는 환경에서는 Java 검증용 `/MT` Core를 별도 build directory로 만든다. 이
variant를 공통 CI runtime이나 다른 binding의 staged runtime과 섞지 않으며, Java
계획 문서에서 별도 증적으로 기록한다.

Windows package 입력과 결과는 다음 경로를 사용한다.

- .NET: `ZLinkWindowsX64NativeRoot=core/build/windows-x64/install/bin`, 결과는
  `.artifacts/windows/dotnet/package/`
- C++: `core/build/windows-x64/install/`와 CMake install 결과
  `.artifacts/windows/cpp/package/`
- Go: `bindings/go/native/windows-x86_64/`, 결과는 `.artifacts/windows/go/package/`
- Java: `core/build/windows-x64/install/bin/zlink.dll`을 사용하는 version-only consumer
- Node.js: `bindings/node/prebuilds/win32-x64/`, 결과는 `.artifacts/windows/node/package/`
- Python: wheel의 `native/windows-x86_64/zlink.dll`, 결과는 `.artifacts/windows/python/wheel/`
- Rust: crate의 `native/windows-x86_64/`, 결과는 `.artifacts/windows/rust/`

Windows native package 생성 절차를 통합할 때는 이 경로와 언어별 version pinning을 함께
갱신한다. 현재 Windows 성능 실행 결과의 상태와 실패 원인은
`doc/perf/perf/core-0.10.0/` 아래의 개별 measurement sheet와 `log/`가 소유한다.

## Core runtime 동기화

`native/sync-local-core-libs.sh`는 `core/build/lib`의 현재 `0.10.1` runtime과
`core/include`를 binding 작업 디렉터리에 복사한다.

```bash
scripts/local-package/native/sync-local-core-libs.sh
```

이 파일들은 local build 입력이다. release package를 만들 때는 script가
생성한 native 파일을 별도로 commit하지 않는다.
