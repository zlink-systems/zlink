# Core 0.10.0 Rust bindings 성능 개선 계획

> [재사용 템플릿](../../bindings-library-performance-improvement-plan-template.ko.md) 기반 문서.
> 진행 로그와 중간 결과는 이 파일에 작성하지 않고 측정 시트와 report에 기록한다.

## 1. 작업 범위

| 항목 | 값 |
|------|-----|
| Core | `0.10.0` |
| binding | `Rust` |
| C baseline | `bindings/c/perf` |
| binding runner | `bindings/rust/perf` |
| 기준 branch | `main` |
| 측정 기록 | 이 문서의 `측정 시트` 절과 `log/` |
| 상태 | `planned` |

Codex Cloud 환경에는 Rust toolchain, native build, package consumer 검증 도구를 고정한다.
작업 시작 시 `AGENTS.md`와 세 perf policy를 읽고 지정 commit에서 실행한다.

## 2. 기준 확인

Core 선언과 runtime, Rust native artifact의 버전이 모두 `0.10.0`인지 확인한다. C와 Rust의
runner inventory와 message ownership·lifetime·polling 의미를 비교한다. 같은 조건의 C와
Rust report가 complete일 때만 paired 결과로 인정한다.

## 3. 개선 규칙

Rust safe public API와 ownership 계약을 유지한다. allocation, copy, FFI boundary, polling
비용을 binding 내부에서 줄인다. unsafe 우회, C API 직접 호출, sleep·retry·timeout 변경,
측정 대상 축소는 성능 개선으로 인정하지 않는다.

## 4. 측정 시트

### 4.1 Manifest

| phase | run_id | paired_run_id | commit | core runtime | host | toolchain | command | ccu | status |
|-------|--------|---------------|--------|--------------|------|-----------|---------|-----|--------|
| `baseline` | | | | | | | | `[100 for STREAM]` | `planned` |

### 4.2 Cell results

| suite | pattern | transport | size | C report | Rust report | C throughput | Rust throughput | ratio | C avg latency | Rust avg latency | status | reason |
|-------|---------|-----------|------|----------|-------------|--------------|-----------------|-------|----------------|-----------------|--------|--------|
| `single` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |
| `multi` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |

Multi `STREAM`은 CCU `100`으로만 측정한다. 비교할 항목만 짧게 C와 Rust를 paired 실행하고,
개선 전후에도 같은 조건으로 반복한다.

### 4.3 Candidate verification

| candidate | changed files | hypothesis | result | adopted | evidence |
|-----------|---------------|------------|--------|---------|----------|
| | | | | `미정` | |

## 5. 완료 기준

Rust unit·contract·package consumer 검증과 paired throughput·latency gate가 모두 완료되어야
한다. 미달·보류 셀은 완료로 바꾸지 않는다.
