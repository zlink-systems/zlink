# Core 0.10.0 Java bindings 성능 개선 계획

> [재사용 템플릿](../../bindings-library-performance-improvement-plan-template.ko.md) 기반 문서.
> 진행 로그와 중간 결과는 이 파일에 작성하지 않고 별도 측정 시트에 기록한다.

## 1. 작업 범위

| 항목 | 값 |
|------|-----|
| Core | `0.10.0` |
| binding | `Java` |
| C baseline | `bindings/c/perf` |
| binding runner | `bindings/java/perf` |
| 기준 branch | `main` |
| 측정 기록 | 이 문서의 `측정 시트` 절과 `log/` |
| 상태 | `planned` |

Codex Cloud에서는 원격 `main`의 지정 commit을 checkout한다. Java/Gradle toolchain과
local package 경로를 environment setup에 고정하고, 작업 시작 시 세 perf policy와
`AGENTS.md`를 읽는다.

## 2. 기준 확인

버전 선언, Core runtime, Java package native runtime이 모두 `0.10.0`인지 확인한다.
C와 Java의 pattern·transport·size·option inventory를 비교하고, 각 paired report가
`status: complete`일 때만 ratio를 계산한다.

## 3. 개선 규칙

Java public API와 documented lifecycle을 유지한다. JNI 경계, allocation, copy, callback,
poller 비용을 binding 내부에서 개선하며, busy wait·sleep·retry·timeout·HWM 변경으로
측정 실패를 숨기지 않는다. Core 변경 시 local Core library를 재배포한 뒤 Java package와
테스트를 다시 검증한다.

## 4. 측정 시트

### 4.1 Manifest

| phase | run_id | paired_run_id | commit | core runtime | host | toolchain | command | ccu | status |
|-------|--------|---------------|--------|--------------|------|-----------|---------|-----|--------|
| `baseline` | | | | | | | | `[100 for STREAM]` | `planned` |

### 4.2 Cell results

| suite | pattern | transport | size | C report | Java report | C throughput | Java throughput | ratio | C avg latency | Java avg latency | status | reason |
|-------|---------|-----------|------|----------|-------------|--------------|-----------------|-------|----------------|-----------------|--------|--------|
| `single` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |
| `multi` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |

Multi `STREAM`은 CCU `100`으로만 측정한다. 비교할 항목만 짧게 C와 Java를 paired
실행하고, 개선 전후에도 같은 조건으로 반복한다.

### 4.3 Candidate verification

| candidate | changed files | hypothesis | result | adopted | evidence |
|-----------|---------------|------------|--------|---------|----------|
| | | | | `미정` | |

## 5. 완료 기준

inventory, paired report, manifest, ratio, latency, Java unit/contract/integration test와
package consumer 검증이 완료되어야 한다. 이 문서에는 최종 상태만 기록한다.
