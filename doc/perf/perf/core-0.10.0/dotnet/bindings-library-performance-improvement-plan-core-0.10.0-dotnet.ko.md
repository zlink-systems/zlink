# Core 0.10.0 .NET bindings 성능 개선 계획

> 재사용 템플릿: [bindings 성능 개선 실행 문서 템플릿](../../bindings-library-performance-improvement-plan-template.ko.md)
>
> 이 계획 문서에는 진행 로그와 중간 결과를 작성하지 않는다. 결과는 별도 측정 시트와
> complete report에 기록한다.

## 1. 작업 범위

| 항목 | 값 |
|------|-----|
| Core | `0.10.0` |
| binding | `.NET` |
| C baseline | `bindings/c/perf` |
| binding runner | `bindings/dotnet/perf` |
| 기준 branch | `main` |
| 측정 기록 | 이 문서의 `측정 시트` 절과 `log/` |
| 상태 | `planned` |

Codex Cloud는 push된 `main` commit에서 실행한다. `AGENTS.md`와 세 perf policy 문서를
먼저 읽고, `dotnet` SDK·native runtime·package consumer 도구는 environment setup으로
고정한다. Cloud 작업 중 branch 생성·전환·merge·push는 별도 요청 없이는 하지 않는다.

## 2. 기준 확인

- 버전 선언과 실제 Core runtime이 모두 `0.10.0`인지 확인한다.
- C와 .NET의 `ALL` inventory와 지원 option을 대조한다.
- public .NET API의 payload, ownership, completion 의미가 C와 같은지 확인한다.
- C report 직후 같은 조건의 .NET report를 실행하고 둘 다 complete일 때만 비교한다.

## 3. 개선 규칙

- public .NET API만 사용하며 reflection으로 binding 내부 멤버에 접근하지 않는다.
- managed/native transition, allocation, copy, callback 비용은 binding 내부에서 개선한다.
- timeout 증가, sleep·retry 추가, client 축소, HWM 튜닝으로 실패를 감추지 않는다.
- Core 결함은 회귀 테스트, local runtime 재배포, .NET 재검증 순서로 처리한다.

## 4. 측정 시트

### 4.1 Manifest

| phase | run_id | paired_run_id | commit | core runtime | host | toolchain | command | ccu | status |
|-------|--------|---------------|--------|--------------|------|-----------|---------|-----|--------|
| `baseline` | | | | | | | | `[100 for STREAM]` | `planned` |

### 4.2 Cell results

| suite | pattern | transport | size | C report | .NET report | C throughput | .NET throughput | ratio | C avg latency | .NET avg latency | status | reason |
|-------|---------|-----------|------|----------|-------------|--------------|----------------|-------|----------------|-----------------|--------|--------|
| `single` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |
| `multi` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |

Multi `STREAM`은 CCU `100`으로만 측정한다. 비교할 항목만 짧게 C와 .NET을 paired
실행하고, 개선 전후에도 같은 조건으로 반복한다.

### 4.3 Candidate verification

| candidate | changed files | hypothesis | result | adopted | evidence |
|-----------|---------------|------------|--------|---------|----------|
| | | | | `미정` | |

## 5. 완료 기준

모든 대상 셀이 complete paired report와 manifest를 가지며 throughput·latency gate와
.NET 단위·통합·package consumer 검증을 통과해야 한다. 계획 문서의 상태만 최종적으로
`complete`로 바꾼다.
