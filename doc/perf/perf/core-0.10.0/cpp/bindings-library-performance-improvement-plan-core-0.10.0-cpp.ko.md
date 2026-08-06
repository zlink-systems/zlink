# Core 0.10.0 C++ bindings 성능 개선 계획

> 이 문서는 [재사용 템플릿](../../bindings-library-performance-improvement-plan-template.ko.md)을
> Core 0.10.0과 C++ binding에 맞춰 복사한 실행 계획이다.
>
> 이 문서에는 진행 로그를 작성하지 않는다. 측정값은 아래 측정 시트와 runner report에
> 기록한다. 진행 로그와 실패 이력은 이 문서가 있는 폴더의 `log/`에 기록한다.

## 1. 작업 범위

| 항목 | 값 |
|------|-----|
| Core | `0.10.0` |
| binding | `C++` |
| C baseline | `bindings/c/perf` |
| binding runner | `bindings/cpp/perf` |
| 기준 branch | `main` |
| 측정 기록 | 이 문서의 `측정 시트` 절과 `log/` |
| 상태 | `planned` |

Codex Cloud에서는 원격 `main`의 지정 commit에서 시작한다. 첫 프롬프트에 `AGENTS.md`,
`doc/perf/PERF_POLICY.md`, `doc/perf/PERF_SINGLE_TEST_POLICY.md`,
`doc/perf/PERF_MULTI_TEST_POLICY.md`를 읽고, 이 문서에는 로그를 쓰지 말라는 조건을
명시한다. Cloud 환경의 setup script에는 Core CMake build와 C++ package build에
필요한 도구만 고정한다.

## 2. 기준 확인

- `VERSION`, `core/CMakeLists.txt`, `core/include/zlink.h`가 모두 `0.10.0`인지 확인한다.
- 현재 `core/build`를 다시 빌드하고 실제 runtime 경로를 기록한다.
- C++ runner의 `ALL` inventory를 C runner와 대조한다.
- 같은 suite, pattern, transport, size, duration, runs, client 수로 C를 먼저 실행한다.
- C와 C++ report가 모두 `status: complete`일 때만 비율을 기록한다.

## 3. 개선 규칙

- C++ public API의 일반 호출 경로를 유지한다.
- private API, C API 직접 호출, perf 전용 API, 특정 size 전용 우회는 사용하지 않는다.
- send/receive 순서, timeout, sleep, retry, HWM을 결과에 유리하게 바꾸지 않는다.
- allocation, copy, dispatch, poller, ownership 비용은 C++ binding 내부에서 줄인다.
- Core 변경이 필요하면 Core 회귀 테스트와 local runtime 재배포 후 paired 측정을 다시 한다.

## 4. 측정 시트

### 4.1 Manifest

| phase | run_id | paired_run_id | commit | core runtime | host | toolchain | command | ccu | status |
|-------|--------|---------------|--------|--------------|------|-----------|---------|-----|--------|
| `baseline` | | | | | | | | `[100 for STREAM]` | `planned` |

### 4.2 Cell results

| suite | pattern | transport | size | C report | C++ report | C throughput | C++ throughput | ratio | C avg latency | C++ avg latency | status | reason |
|-------|---------|-----------|------|----------|------------|--------------|----------------|-------|----------------|----------------|--------|--------|
| `single` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |
| `multi` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |

Multi `STREAM`은 CCU `100`으로만 측정한다. 전체 baseline을 장시간 실행하지 않고,
비교할 `suite + pattern + transport + size`만 선택해 C와 C++를 같은 짧은 조건으로
연속 측정한다. 개선 후에도 동일한 선택 조건으로 다시 측정한다.

### 4.3 Candidate verification

| candidate | changed files | hypothesis | result | adopted | evidence |
|-----------|---------------|------------|--------|---------|----------|
| | | | | `미정` | |

## 5. 완료 기준

모든 inventory 셀의 paired report, 조건 manifest, throughput·latency 판정, C++ 테스트와
package consumer 검증이 완료되어야 한다. `미측정`, `미달`, `보류` 셀이 남아 있으면
완료로 표시하지 않는다.
