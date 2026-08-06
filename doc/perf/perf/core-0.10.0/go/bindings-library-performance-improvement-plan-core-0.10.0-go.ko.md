# Core 0.10.0 Go bindings 성능 개선 계획

> [재사용 템플릿](../../bindings-library-performance-improvement-plan-template.ko.md) 기반 문서.
> 계획 문서에는 진행 로그와 중간 결과를 작성하지 않는다.

## 1. 작업 범위

| 항목 | 값 |
|------|-----|
| Core | `0.10.0` |
| binding | `Go` |
| C baseline | `bindings/c/perf` |
| binding runner | `bindings/go/perf` |
| 기준 branch | `main` |
| 측정 기록 | 이 문서의 `측정 시트` 절과 `log/` |
| 상태 | `planned` |

Codex Cloud 작업은 원격 `main` commit에서 시작한다. Go toolchain, native runtime, package
consumer test를 setup script로 고정하고, 측정 시트와 report 외에는 결과를 기록하지 않는다.

## 2. 기준 확인

Core 버전과 실제 runtime을 확인한 뒤 C와 Go runner의 inventory를 대조한다. GOMAXPROCS,
CGO, client 수, duration, runs 등 측정 조건은 C와 binding manifest에 모두 기록한다.
paired report가 complete일 때만 ratio와 latency를 계산한다.

## 3. 개선 규칙

Go public API의 ownership과 blocking 의미를 유지한다. allocation, cgo transition, copy,
goroutine scheduling 비용을 내부에서 개선한다. `runtime.Gosched`, sleep, retry, timeout,
client 수 변경을 통과용 우회로 사용하지 않는다. Core 변경 후 local runtime을 재배포하고
Go package consumer와 perf를 다시 검증한다.

## 4. 측정 시트

### 4.1 Manifest

| phase | run_id | paired_run_id | commit | core runtime | host | toolchain | command | ccu | status |
|-------|--------|---------------|--------|--------------|------|-----------|---------|-----|--------|
| `baseline` | | | | | | | | `[100 for STREAM]` | `planned` |

### 4.2 Cell results

| suite | pattern | transport | size | C report | Go report | C throughput | Go throughput | ratio | C avg latency | Go avg latency | status | reason |
|-------|---------|-----------|------|----------|------------|--------------|---------------|-------|----------------|---------------|--------|--------|
| `single` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |
| `multi` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |

Multi `STREAM`은 CCU `100`으로만 측정한다. 비교할 항목만 짧게 C와 Go를 paired 실행하고,
개선 전후에도 같은 조건으로 반복한다.

### 4.3 Candidate verification

| candidate | changed files | hypothesis | result | adopted | evidence |
|-----------|---------------|------------|--------|---------|----------|
| | | | | `미정` | |

## 5. 완료 기준

Go 단위·contract·package consumer 검증과 모든 paired performance gate가 완료되어야 한다.
상태는 마지막에만 `complete`로 변경한다.
