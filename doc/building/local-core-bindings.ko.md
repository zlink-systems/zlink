# 로컬 Core로 바인딩 링크 — 테스트·perf 참조

릴리스 artifact 없이 **작업 중인(브랜치) Core**로 각 언어 바인딩을 링크해 계약 테스트와 perf를
돌리는 방법을 모은 참조다. 실제 동작의 정본은 인용한 스크립트이며, 이 문서는 "무엇을 어떤 순서로,
어떤 환경변수로" 부르는지의 지도다.

핵심 구분:

| 목적 | Core 소스 | 무엇을 가리키나 | 진입점 |
|------|-----------|-----------------|--------|
| **계약 테스트** | 라이브 `core/build-dev` (매 빌드 최신) | 헤더 `core/include` + `core/build-dev/lib/libzlink` | `scripts/gate/bindings-gate.sh` |
| **perf 측정** | **고정** install prefix (측정 중 불변) | `ZLINK_CORE_PACKAGE_PREFIX`가 가리키는 prefix | `scripts/perf/perf-ticket.sh` |

테스트는 "지금 소스"를, perf는 "고정된 산출물"을 재는 것이 원칙이다. 그래서 링크 방식이 다르다.

## 1. 바인딩 계약 테스트 — 라이브 `core/build-dev`

Core 소스를 고친 뒤(예: 이번 브랜치가 추가한 새 공개 심볼) 바인딩이 그 심볼을 보게 하려면 릴리스
패키지나 `build-wsl.sh`가 필요 없다. 각 바인딩 `tests/run_tests.sh`가 `ZLINK_CORE_SOURCE=local`과
로컬 Core의 include/lib 경로를 받아 **`core/build-dev` 트리에 직접 링크**한다.

절차:

```bash
# 1) 브랜치 Core를 dev로 빌드 (새 심볼이 core/include + core/build-dev/lib/libzlink 에 들어간다)
JOBS=4 scripts/build-core.sh dev

# 2) 모든 바인딩 계약 스위트 (core/build-dev 상대, samples lock 뒤 직렬)
scripts/gate/bindings-gate.sh <tag>
```

`bindings-gate.sh`는 언어별로 아래 환경을 세워 `tests/run_tests.sh`를 부른다
(`scripts/gate/common.sh`: `CORE_LIB=$Z/core/build-dev/lib`, `CORE_VER`=`VERSION`의 `LIBZLINK_VERSION`).
개별 언어만 돌릴 때 이 표 그대로 쓴다.

| 바인딩 | 로컬 Core 링크 환경변수 |
|--------|--------------------------|
| c | `ZLINK_C_CORE_BUILD_DIR=core/build-dev ZLINK_CORE_INCLUDE_DIR=core/include` |
| cpp | `ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR=core/build-dev ZLINK_BUILD_JOBS=4` |
| dotnet | `ZLINK_LIBRARY_PATH=core/build-dev/lib` |
| java | `ZLINK_CORE_SOURCE=local ZLINK_CORE_INCLUDE_DIR=core/include ZLINK_CORE_LIB_DIR=core/build-dev/lib` |
| node | `ZLINK_CORE_SOURCE=local ZLINK_LIBRARY_PATH=core/build-dev/lib/libzlink.so.<CORE_VER> LD_LIBRARY_PATH=core/build-dev/lib` |
| python | `ZLINK_CORE_SOURCE=local ZLINK_LIBRARY_PATH=core/build-dev/lib/libzlink.so PYTHON_EXECUTABLE=<venv python>` |
| go | (환경 없음 — cgo가 트리 상대로 링크) |
| rust | `LD_LIBRARY_PATH=core/build-dev/lib` |

예: 단일 언어

```bash
JOBS=4 scripts/build-core.sh dev
ZLINK_CORE_SOURCE=local ZLINK_CPP_CORE_BUILD_DIR="$PWD/core/build-dev" ZLINK_BUILD_JOBS=4 \
  bash bindings/cpp/tests/run_tests.sh
```

- `ZLINK_GATE_CORE_LIB` / `ZLINK_GATE_CORE_VERSION`으로 릴리스 prefix(예:
  `~/.cache/zlink/core/<ver>/<platform>/lib`)를 가리키게 오버라이드할 수 있다. 릴리스와 비교할 때만
  쓰고, 브랜치의 새 심볼을 테스트할 때는 기본값(`core/build-dev`)을 쓴다.
- `bindings/*/include`는 `core/include`의 **raw header mirror**다. 공개 헤더에 심볼을 추가하면
  mirror를 맞춰야 한다(`python3 scripts/local-package/sync-version.py --check`, 필요 시 `--write`).

## 2. perf 측정 — 고정 Core prefix

perf는 측정 중 Core가 바뀌면 안 된다. `perf-ticket.sh`는 `ZLINK_CORE_SOURCE=local`을
**쓰지 말라고 경고**한다 — 그 경우 러너가 `core/build`를 자동 재빌드해 측정 도중 Core가 바뀌고
고정 prefix가 아닌 dev 빌드를 재게 된다(`scripts/perf/perf-ticket.sh` 주석). 대신 **고정 install
prefix**를 만들어 `ZLINK_CORE_PACKAGE_PREFIX`로 가리킨다.

브랜치 Core를 재는(=after) 절차:

```bash
# 1) 브랜치 Core를 release로 빌드 (LTO, 출하 라이브러리)
JOBS=4 scripts/build-core.sh release

# 2) 고정 prefix로 materialize — 공식 릴리스 캐시를 덮어쓰지 않도록 별도 위치를 쓴다
ZLINK_CORE_CACHE_DIR=<브랜치-전용-캐시> \
  scripts/gate/materialize-local-core-prefix.sh core/build
#   -> <브랜치-전용-캐시>/<ver>/<platform> 에 prefix 생성

# 3) runner를 한 번만 띄우고(직렬성의 근거), 티켓으로 측정 제출
nohup scripts/perf/perf-queue-runner.sh >/dev/null 2>&1 &
ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=<브랜치-전용-캐시>/<ver>/<platform> \
  scripts/perf/perf-ticket.sh submit -p 1 -d "issue-XX after" -- <측정 명령>
```

- `perf-ticket.sh`는 제출 시점의 `ZLINK_CORE_SOURCE`·`ZLINK_CORE_PACKAGE_PREFIX`를 티켓에 그대로
  기록한다. **직렬성은 "runner가 하나"라는 사실에서 나온다**(동시 perf 금지). 한 번에 하나만 잰다.
- baseline(=before)은 이미 만든 공식/기준 prefix를 같은 방식으로 가리켜 잰다. C canonical baseline과
  비교 방식은 perf 계획서(`doc/perf/perf/**`)가 소유한다.
- `materialize-local-core-prefix.sh`는 기본 캐시 루트(`~/.cache/zlink/core`)에 `<ver>/<platform>`으로
  쓴다. 브랜치 버전이 릴리스와 같으면(예: 아직 범프 전) **공식 캐시를 덮어쓰므로**, `ZLINK_CORE_CACHE_DIR`
  로 브랜치 전용 위치를 지정해 분리한다.

## 3. 소유 관계

- 링크 동작의 정본: `scripts/gate/bindings-gate.sh`·`scripts/gate/common.sh`(테스트),
  `scripts/perf/perf-ticket.sh`·`perf-queue-runner.sh`(perf).
- 빌드 트리·모드(`dev`/`release`/`release-gate`)는 `scripts/build-core.sh`와
  [`build-guide.ko.md`](build-guide.ko.md)가 소유한다.
- 로컬 패키지 공유 캐시(§4.1)·버전 동기화는 `scripts/local-package/`와
  [개발 워크플로](../principal/dev/development-workflow.ko.md)가 소유한다.
- 이 문서는 그 위에서 "릴리스 없이 로컬 Core로 바인딩을 링크하는 두 경로(테스트·perf)"만 모은
  참조다.
