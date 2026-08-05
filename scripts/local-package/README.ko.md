# Local package

이 디렉터리는 외부 registry에 publish하지 않고 Core와 first-party binding을
같은 `0.9.0` source 기준으로 빌드하는 경로다. 기본 출력은
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
.artifacts/wsl/install/zlink-core/0.9.0/
  include/
  lib/libzlink.so
  lib/libzlink.so.0
  lib/libzlink.so.0.9.0
  share/zlink/core-package-provenance.json
```

`0.9.0`은 release/package version이다. native runtime의 SONAME도 같은
release line에 맞춰 `libzlink.so.0`으로 생성한다. 외부 dependency의 버전은
이 정책의 대상이 아니다.

## binding별 출력

- C: `.artifacts/wsl/c/zlink-c-0.9.0.tar.gz`
- C++: `.artifacts/wsl/install/zlink-cpp/0.9.0/`
- .NET: `.artifacts/wsl/nuget/Systems.Zlink.0.9.0.nupkg`
- Go: `.artifacts/wsl/go/zlink-go-0.9.0.tar.gz`
- Java: `.artifacts/wsl/maven/systems/zlink/zlink/0.9.0/`
- Node.js: `.artifacts/wsl/npm/zlink-systems-zlink-0.9.0.tgz`
- Python: `.artifacts/wsl/python/zlink-0.9.0-*.whl` 및 source archive
- Rust: `.artifacts/wsl/rust/zlink-0.9.0.crate`

Go의 public module path는 `zlink.systems/zlink`이며, release version과
import path를 분리한다. 모든 binding package는 Core provenance에 기록된
`0.9.0` runtime과 public header를 사용한다.

## Core runtime 동기화

`native/sync-local-core-libs.sh`는 `core/build/lib`의 현재 `0.9.0` runtime과
`core/include`를 binding 작업 디렉터리에 복사한다.

```bash
scripts/local-package/native/sync-local-core-libs.sh
```

이 파일들은 local build 입력이다. release package를 만들 때는 script가
생성한 native 파일을 별도로 commit하지 않는다.
