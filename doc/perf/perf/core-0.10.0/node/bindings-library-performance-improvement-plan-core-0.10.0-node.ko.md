# Core 0.10.0 Node bindings 성능 개선 계획

> [재사용 템플릿](../../bindings-library-performance-improvement-plan-template.ko.md) 기반 문서.
> 이 파일에는 진행 로그를 쓰지 않으며, 측정값은 별도 시트와 complete report에만 기록한다.

## 1. 작업 범위

| 항목 | 값 |
|------|-----|
| Core | `0.10.0` |
| binding | `Node` |
| C baseline | `bindings/c/perf` |
| binding runner | `bindings/node/perf` |
| 기준 branch | `main` |
| 측정 기록 | 이 문서의 `측정 시트` 절과 `log/` |
| 상태 | `planned` |

Codex Cloud에서는 push된 `main` commit에서 시작한다. Node.js, native build, package
consumer 도구를 environment setup으로 고정하고, `AGENTS.md`와 perf policy를 먼저 읽는다.

## 2. 기준 확인

Core 선언·runtime·Node package native artifact가 `0.10.0`인지 확인한다. C와 Node runner의
inventory, payload 의미, receive 모델, option 적용 여부를 대조한다. C와 Node를 같은 조건으로
연속 실행하고 complete report만 비교 근거로 사용한다.

## 3. 개선 규칙

Node public API와 event loop 의미를 유지한다. allocation, Buffer copy, native transition,
dispatch 비용은 binding 내부에서 줄인다. callback 우회, raw C API 직접 호출, busy wait,
sleep·retry·timeout 조정, 입력 size 전용 최적화는 금지한다.

## 4. 측정 시트

### 4.1 Manifest

| phase | run_id | paired_run_id | commit | core runtime | host | toolchain | command | ccu | status |
|-------|--------|---------------|--------|--------------|------|-----------|---------|-----|--------|
| `baseline` | | | | | | | | `[100 for STREAM]` | `planned` |

### 4.2 Cell results

| suite | pattern | transport | size | C report | Node report | C throughput | Node throughput | ratio | C avg latency | Node avg latency | status | reason |
|-------|---------|-----------|------|----------|-------------|--------------|----------------|-------|----------------|-----------------|--------|--------|
| `single` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |
| `multi` | `[pattern]` | `[transport]` | `[bytes]` | | | | | | | | `미측정` | |

Multi `STREAM`은 CCU `100`으로만 측정한다. 비교할 항목만 짧게 C와 Node를 paired
실행하고, 개선 전후에도 같은 조건으로 반복한다.

### 4.3 Candidate verification

| candidate | changed files | hypothesis | result | adopted | evidence |
|-----------|---------------|------------|--------|---------|----------|
| | | | | `미정` | |

## 5. 완료 기준

모든 대상 셀의 paired complete report, Node test/consumer 검증, throughput·latency 판정이
측정 시트에 있고 미달 또는 보류 셀이 없어야 한다. 진행 경과는 이 문서에 추가하지 않는다.
