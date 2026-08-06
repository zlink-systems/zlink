# bindings 성능 개선 실행 문서 템플릿

> 이 문서는 새 Core 버전과 한 언어의 bindings 성능 개선 작업을 시작할 때 복사하는
> 재사용 템플릿이다. 대괄호로 표시한 값만 새 작업에 맞게 채운다.
>
> 이 문서에는 진행 로그, 중간 결과, 회의 메모, 실패 이력을 작성하지 않는다.
> 측정값은 문서의 측정 시트와 runner report에 기록한다. 진행 로그와 실패 이력은
> 문서가 있는 언어 폴더의 `log/`에 별도 파일로 기록한다. 완료 후에는 최종 판단과
> 근거 링크만 계획 문서에 남긴다.

## 1. 작업 식별 정보

| 항목 | 값 |
|------|-----|
| 대상 Core 버전 | `[예: 0.10.0]` |
| 대상 binding | `[예: C++]` |
| 기준 branch | `main` |
| 기준 commit | `[측정 시작 전 commit SHA]` |
| 계획 문서 | `[이 파일의 복사본 경로]` |
| 측정 시트 | `[측정 시트 경로]` |
| 상태 | `planned` |

### 1.1 Codex Cloud 실행 조건

Codex Cloud에서 작업할 때는 원격 저장소에 push된 commit과 명시한 branch를 기준으로
시작한다. 로컬 WSL의 미커밋 변경과 로컬 package cache는 Cloud 환경에 자동으로
전달된다고 가정하지 않는다.

Cloud 작업 첫 프롬프트에는 다음 조건을 포함한다.

```text
main 브랜치와 지정한 commit을 기준으로 작업한다.
먼저 AGENTS.md, doc/perf/PERF_POLICY.md,
doc/perf/PERF_SINGLE_TEST_POLICY.md,
doc/perf/PERF_MULTI_TEST_POLICY.md를 읽는다.
계획 문서에는 진행 로그나 중간 결과를 쓰지 않는다.
측정값은 지정한 측정 시트와 runner report에 기록한다.
진행 로그와 실패 이력은 문서가 있는 언어 폴더의 `log/`에 기록한다.
사용자가 명시하지 않은 branch 생성·전환·merge·push는 하지 않는다.
```

Cloud environment에는 해당 언어의 toolchain, Core build 의존성, binding package
의존성을 setup script로 고정한다. Agent phase의 인터넷 접근, secret, cache 상태는
측정 manifest에 기록한다.

## 2. 기준과 범위

대상 Core 버전이 아래 세 위치에서 동일한지 먼저 확인한다.

- `VERSION`
- `core/CMakeLists.txt`
- `core/include/zlink.h`

기준 성능은 같은 Core runtime으로 실행한 `bindings/c/perf`의 결과다. 비교 단위는
`suite + pattern + transport + message size + duration + runs + client 수 + metric`의
완전한 조합이다.

```text
binding ratio (%) = binding throughput / C throughput * 100
latency ratio     = binding average latency / C average latency
```

이전 버전의 결과, 이전 계획 문서의 숫자, `doc/perf/perf/log/`의 기록은 새 기준값이나
완료 근거로 사용하지 않는다. 필요하면 참고 자료로만 열람한다.

### 2.1 짧은 paired baseline

전체 pattern·transport·size를 한 번에 장시간 실행해 baseline을 만들지 않는다. 매번
비교할 `suite + pattern + transport + message size`만 선택하고 C와 대상 binding을
연속으로 실행한다.

```text
baseline = C(selected case) -> binding(selected case)
after    = C(selected case) -> binding(selected case, changed code)
```

`baseline`과 `after`는 선택 범위, duration, runs, client 수, I/O thread, HWM, timeout,
CPU 조건을 동일하게 유지한다. 성능 개선 여부는 같은 선택 항목의 paired ratio와
latency를 비교해 판단한다. Multi `STREAM`은 CCU를 `100`으로 고정하며 `10000` CCU
측정은 수행하지 않는다.

## 3. 측정 전 확인

1. 현재 branch가 `main`인지 확인한다.
2. 기준 commit과 worktree 상태를 manifest에 기록한다.
3. `core/build`를 현재 Core 소스로 다시 빌드한다.
4. Core runtime의 실제 경로와 public version을 확인한다.
5. C runner와 대상 binding runner의 pattern, transport, size, option inventory를
   비교한다.
6. 하나라도 다르면 paired 측정을 시작하지 않고 `measurement gap`으로 표시한다.
7. C smoke와 binding smoke가 모두 `status: complete`인지 확인한다.
8. 이번 비교에 필요한 선택 항목만 짧게 실행하고 전체 baseline은 실행하지 않는다.

## 4. 측정 조건

정책 문서의 기본값을 사용한다. 조건을 바꾸려면 C와 binding에 동일하게 적용하고
측정 시트의 manifest에 남긴다.

| Suite | 기본 message size | 기본 반복 |
|-------|-------------------|-----------|
| Single | 64, 256, 1024, 65536, 131072, 262144 bytes | smoke 1회, 탐색 3회, 최종 5회 |
| Multi | 64, 256, 1024, 4096, 65536, 131072 bytes | 선택 항목만 smoke 1회, 탐색 3회, 최종 5회 |

Multi `STREAM`은 선택한 케이스마다 `CCU=100`을 사용한다.

정확한 pattern과 transport는 현재 C runner의 `ALL` inventory를 기준으로 대상
binding runner에 대응시킨다. runner에 없는 경우 임의로 추가하지 않는다.

## 5. 작업 순서

### 5.1 Inventory

- C와 binding runner의 지원 범위를 표로 대조한다.
- 지원하지 않는 public contract는 새 public API로 우회하지 않는다.
- 정책과 runner가 어긋나면 먼저 정책 또는 runner 정합성을 해결한다.

### 5.2 Baseline

- 한 번에 비교할 `suite + pattern + transport + size`만 선택한다.
- C를 먼저 실행하고 같은 조건으로 binding을 바로 실행한다.
- 두 report가 모두 complete일 때만 비율을 계산한다.
- 전체 matrix를 장시간 실행해 baseline을 만들지 않는다.

### 5.3 개선

- binding의 일반 public API 경로 안에서만 개선한다.
- private API, C API 직접 호출, perf 전용 public API, 입력값 전용 우회는 금지한다.
- timeout, sleep, retry, client 수 축소, HWM 숫자 조정으로 실패를 숨기지 않는다.
- Core 결함이면 Core 회귀 테스트와 local runtime 재배포 후 binding을 다시 측정한다.

### 5.4 검증

- 변경 전후의 paired C 결과를 사용한다.
- 개선 전과 개선 후에 동일한 선택 항목을 같은 짧은 조건으로 다시 실행한다.
- 대상 셀의 throughput과 latency뿐 아니라 대상 외 대표 셀의 회귀도 확인한다.
- 단위 테스트, 통합 테스트, package consumer test를 실행한다.
- 최종 판단만 계획 문서의 완료 기준에 반영한다.

## 6. 판정 규칙

각 셀은 `미측정`, `측정 gap`, `미달`, `보류`, `통과` 중 하나다.

- `미측정`: report가 아직 없음
- `측정 gap`: C에는 있지만 binding runner 또는 public contract에 없음
- `미달`: 측정은 완료됐지만 목표를 충족하지 못함
- `보류`: 결과가 불완전하거나 재현 조건이 깨져 판정하지 않음
- `통과`: complete report, 조건 일치, throughput·latency gate를 모두 충족함

완료 시 모든 대상 셀이 `통과`이거나, 계약상 제외 사유가 측정 시트에 명확히 기록된
`측정 gap`이어야 한다. `미달`과 `보류`는 완료로 보지 않는다.

## 7. 측정 시트

아래 표는 별도 측정 시트의 기본 구조다. 실행할 때는 이 섹션을 복사하지 말고
`[측정 시트 경로]`의 독립 파일을 갱신한다.

### 7.1 Manifest

| phase | run_id | paired_run_id | commit | core_runtime | host | toolchain | command | ccu | status |
|-------|--------|---------------|--------|--------------|------|-----------|---------|-----|--------|
| `baseline` | `[id]` | `[id]` | `[sha]` | `[absolute path]` | `[host]` | `[versions]` | `[full command]` | `[100 for STREAM]` | `planned` |

### 7.2 Cell result

| phase | suite | pattern | transport | size | C report | binding report | C throughput | binding throughput | ratio | C avg latency | binding avg latency | ccu | status | reason |
|-------|-------|---------|-----------|------|----------|----------------|--------------|--------------------|-------|----------------|--------------------|-----|--------|--------|
| `baseline` | `[single/multi]` | `[pattern]` | `[transport]` | `[bytes]` | `[path]` | `[path]` | | | | | | `[100 for STREAM]` | `미측정` | |

### 7.3 변경 검증

| candidate | 변경 파일 | 가설 | 결과 | 채택 여부 | 근거 report |
|-----------|-----------|------|------|-----------|-------------|
| `[name]` | `[paths]` | `[hypothesis]` | `[result]` | `미정` | `[path]` |

## 8. 최종 완료 기준

- 기준 Core 버전과 runtime이 일치한다.
- C와 binding의 paired report가 모두 complete다.
- 측정 시트에 command, manifest, report, 옵션, 반복값이 있다.
- throughput과 latency gate를 통과한다.
- 대상 외 대표 셀에 허용되지 않은 회귀가 없다.
- 테스트와 package consumer 검증이 통과한다.
- 계획 문서에는 진행 로그가 없고, 최종 판정만 기록되어 있다.
- 변경을 사용자가 요청한 경우에만 commit, push, PR을 수행한다.
