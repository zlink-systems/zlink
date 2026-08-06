# Core 0.10.0 Python bindings 성능 개선 계획

> [재사용 템플릿](../../bindings-library-performance-improvement-plan-template.ko.md) 기반 문서.
> 이 파일에는 진행 로그와 중간 측정값을 작성하지 않는다. 측정 시트와 complete report만
> 결과의 저장 위치로 사용한다.

## 1. 작업 범위

| 항목 | 값 |
|------|-----|
| Core | `0.10.0` |
| binding | `Python` |
| C baseline | `bindings/c/perf` |
| binding runner | `bindings/python/perf` |
| 기준 branch | `main` |
| 측정 기록 | 이 문서의 `측정 시트` 절과 `log/` |
| 상태 | `planned` |

Codex Cloud에서는 원격 `main`의 지정 commit에서 시작한다. Python runtime, FFI/native
artifact, package consumer test를 environment setup에 고정한다. 계획 문서에는 변경 이력이나
실패 원인을 누적하지 않는다.

## 2. 기준 확인

Core 선언과 실제 runtime이 `0.10.0`인지 확인하고 C와 Python runner의 inventory를 대조한다.
GIL, allocation, buffer copy, FFI 경계가 C와 다른 측정 의미를 만들지 않는지 확인한다.
같은 조건의 complete paired report만 비교 기준으로 사용한다.

## 3. 개선 규칙

Python public API와 소유권·수명 계약을 유지한다. 불필요한 Python 객체 생성, buffer copy,
FFI 호출, dispatch 비용을 binding 내부에서 줄인다. raw C API 직접 호출, 측정용 private
helper 공개, sleep·retry·timeout 조정, 입력값 전용 우회는 금지한다.

## 4. 측정 시트

### 4.1 Manifest

| phase | run_id | paired_run_id | commit | core runtime | host | toolchain | command | ccu | status |
|-------|--------|---------------|--------|--------------|------|-----------|---------|-----|--------|
| `baseline` | | | | | | | | `[100 for STREAM]` | `planned` |

### 4.2 Cell results

| suite | pattern | transport | size | C report | Python report | C throughput | Python throughput | ratio | C avg latency | Python avg latency | status | reason |
|-------|---------|-----------|------|----------|---------------|--------------|-------------------|-------|----------------|-------------------|--------|--------|
| `single` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |
| `multi` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |

Multi `STREAM`은 CCU `100`으로만 측정한다. 비교할 항목만 짧게 C와 Python을 paired 실행하고,
개선 전후에도 같은 조건으로 반복한다.

### 4.3 Candidate verification

| candidate | changed files | hypothesis | result | adopted | evidence |
|-----------|---------------|------------|--------|---------|----------|
| | | | | `미정` | |

## 5. 완료 기준

Python unit·native contract·package consumer 검증과 paired throughput·latency gate를 모두
통과해야 한다. 모든 결과는 측정 시트에 기록하고 이 문서에는 최종 상태만 남긴다.
