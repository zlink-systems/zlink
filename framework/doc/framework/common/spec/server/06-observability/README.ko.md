---
title: "Observability"
---

# Observability

[스펙 목차](../README.ko.md) · [다음: 01. Runtime 상태와 운영 진단](01-runtime-monitoring.ko.md)

## 1. 무엇을 관찰할 수 있는가

Application과 운영자는 이 주제의 네 문서로 process 하나가 지금 무엇을 하고 있는지
네 층에서 관찰한다. 지금 이 순간의 완전한 상태(Host가 새 작업을 받을 수 있는지,
여러 node가 Channel message를 주고받는 범위인
[RouteMesh](../00-foundation/02-glossary.ko.md#routemesh)·ClientServer·automatic
fanout 각각이 준비됐는지)는 조회로 확인하고,
그 상태가 바뀌는 순간은 변화 stream으로 관찰한다. 시간에 따라 쌓이는 처리량·대기·
실패·현재 개수는 metric으로 모으고, message 한 건이 어느 처리 단계까지 갔는지와
그 message가 다른 어느 message와 같은 원인에서 이어졌는지는 trace와 두 식별자로
추적한다.

네 문서는 서로 다른 관찰 단위를 소유하며 겹치지 않는다 — 관찰 단위별 소유 경계는
[§2](#2-이-주제의-문서)가 정의한다. 이 기록들은 message 전달·완료 보장이나 routing,
handler 분배, lifecycle 결정을 바꾸지 않는다. Application, 운영 도구와 provider가
서로 다르게 개입한다.

| 주체 | 이 주제에서 하는 일 |
|---|---|
| Application | 기록 수준(diagnostics level), sampling 비율, message size 기록 여부를 설정하고, 등록 이름으로 status를 조회하거나 변화를 관찰한다. |
| Framework | 각 처리 경계에서 status·metric·trace 기록을 만들고 표준 경로로 전달한다. Correlation·flow 식별자를 생성·전파·복원한다. |
| Provider | Structured logger, metric exporter와 telemetry provider를 구성한다. 처리 지연이나 결과 변경은 금지된다. |
| 운영 도구 | 등록하지 않은 process 내부로 개입하지 않고, 공개 조회·구독·structured log만으로 process 상태를 판단한다. |

## 2. 이 주제의 문서

| 주제 안 문서 | 소유 범위 |
|---|---|
| [01. Runtime 상태와 운영 진단](01-runtime-monitoring.ko.md) | 특정 시점의 완전한 status, status 변화 stream, structured log identifier |
| [02. Runtime metric](02-runtime-metrics.ko.md) | 시간에 따라 누적·수집하는 metric의 이름·종류·단위·label |
| [03. Message flow tracing](03-message-flow-tracing.ko.md) | message 한 건의 진행 기록(trace)과 그 attribute·기록 수준 |
| [04. Request correlation](04-flow-correlation.ko.md) | `correlation_id`·`flow_id`·`flow_origin`의 생성·형식·전파·소유권·수명 |

개별 host operation(relocation·shutdown)의 결과는 이 주제가 아니라
[Host relocation 전체 흐름](../05-location-relocation/05-host-relocation-flow.ko.md)이 소유한다.

## 3. 질문으로 찾기

| 질문 | 답이 있는 절 |
|---|---|
| 운영자는 process 전체가 지금 새 작업을 받을 수 있는지 한 번에 어떻게 확인하는가 | [01. Runtime 상태와 운영 진단 「3. Host 상태 — 한 번에 읽는 값」](01-runtime-monitoring.ko.md#3-host-상태--한-번에-읽는-값) |
| RouteMesh·ClientServer·automatic fanout 각각의 준비 상태는 무엇으로 확인하는가 | [01. Runtime 상태와 운영 진단 「5. Topology 상태 — RouteMesh·ClientServer·automatic fanout」](01-runtime-monitoring.ko.md#5-topology-상태--routemeshclientserverautomatic-fanout) |
| 상태가 바뀌는 순간을 놓치지 않으려면 무엇을 관찰하는가, 관찰자가 느리면 어떻게 되는가 | [01. Runtime 상태와 운영 진단 「6. 상태 변화를 관찰한다 — Sequence와 완전한 status」](01-runtime-monitoring.ko.md#6-상태-변화를-관찰한다--sequence와-완전한-status) · [「7. 관찰자가 느릴 때 — source, 합치기, 유실 누계」](01-runtime-monitoring.ko.md#7-관찰자가-느릴-때--source-합치기-유실-누계) |
| 지금 이 Actor·Spot이 어디 있는지 운영 도구로 조회하려면 | [01. Runtime 상태와 운영 진단 「8. Object의 현재 위치 조회」](01-runtime-monitoring.ko.md#8-object의-현재-위치-조회) |
| 상태가 바뀐 이유는 어디서 찾는가 | [01. Runtime 상태와 운영 진단 「9. Structured log」](01-runtime-monitoring.ko.md#9-structured-log) |
| 처리량·대기·실패·현재 개수를 dashboard로 보려면 어떤 이름의 수치를 모으는가 | [02. Runtime metric 「2. 이름과 집계 규칙」](02-runtime-metrics.ko.md#2-이름과-집계-규칙) |
| 같은 계기를 모든 언어의 같은 dashboard·alert로 볼 수 있는 근거는 무엇인가 | [02. Runtime metric 「2. 이름과 집계 규칙」](02-runtime-metrics.ko.md#2-이름과-집계-규칙) · [「10. Label cardinality」](02-runtime-metrics.ko.md#10-label-cardinality) |
| Relocation 한 건이 얼마나 걸렸고 어디서 막혔는지 어떤 수치로 보는가 | [02. Runtime metric 「8. Host relocation과 shutdown」](02-runtime-metrics.ko.md#8-host-relocation과-shutdown) |
| Message 한 건이 어느 처리 단계까지 갔고 어디서 실패했는지 어떻게 추적하는가 | [03. Message flow tracing 「2. 처리 단계」](03-message-flow-tracing.ko.md#2-처리-단계) |
| 이 흐름 기록을 켜고 끄는 비용은 얼마인가, 꺼도 정말 비용이 0인가 | [03. Message flow tracing 「5. 실행 중 기록 수준 변경과 비용 규칙」](03-message-flow-tracing.ko.md#5-실행-중-기록-수준-변경과-비용-규칙) |
| Request와 그 reply는 무엇으로 연결되는가 | [04. Request correlation 「2. 두 식별자의 역할」](04-flow-correlation.ko.md#2-두-식별자의-역할) |
| 여러 message가 같은 원인에서 시작됐다는 것은 무엇으로 아는가 | [04. Request correlation 「5. 전파 규칙」](04-flow-correlation.ko.md#5-전파-규칙) |
| 이 식별자들에 개인정보나 payload가 들어가는가 | [04. Request correlation 「8. 관측과 privacy」](04-flow-correlation.ko.md#8-관측과-privacy) |
| 간헐 실패를 조사할 때 운영자는 무엇부터 켜서 읽는가 | [§4 간헐 실패를 쫓는 순서](#4-간헐-실패를-쫓는-순서) |

## 4. 간헐 실패를 쫓는 순서

간헐 실패를 쫓을 때는 **이미 있는 message tracking과 파일 log를 먼저 켜고 읽는다.**
임시 log를 새로 넣고 재현을 반복하는 방식은 금지한다. 그 방식은 예외 하나를 보려고
재현 주기를 통째로 다시 돌리게 만들고, 정작 원인이 기존 log에 이미 찍혀 있어도
놓친다.

### 4.1 무엇을 먼저 켜는가

| 대상 | 켜는 방법 |
|---|---|
| Message flow(`flow`, `corr` 포함 전 구간 추적) | runtime diagnostics의 message flow mode |
| C++ / .NET spot discovery trace | `ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY` |
| Java / Kotlin stream trace | `ZLINK_JAVA_STREAM_TRACE=1` |
| Sample 서버 log 보존 | .NET `ZLINK_SAMPLE_EVIDENCE_DIR`, JVM `ZLINK_SAMPLE_KEEP_RUN_DIR=1`, Node는 실패 시 자동 |

Sample이 간헐 실패하면 **첫 재현부터** 서버 log를 보존한다. log 없이 돌린 재현은
실패 사실만 남기고 원인은 남기지 않으므로 그 주기는 버려진다.

### 4.2 어떻게 읽는가

먼저 `flow`로 정상 건과 실패 건을 나란히 놓고 **어느 전이에서 끊겼는지** 찾는다.
`flow`는 message 하나를 process 경계 너머까지 잇는 유일한 값이다. Trace 종류를
noise로 보고 grep에서 걸러내면 원인 줄을 그대로 지나친다.

### 4.3 실패는 반드시 flow에 남긴다

Application에 error kind만 돌려주고 원인을 버리는 종결은 만들지 않는다. 원인을
남기지 않은 실패는 재현으로만 추적할 수 있고, 재현 주기가 곧 조사 비용이 된다.
실패·거부·abort 같은 종결은 [03. Message flow tracing](03-message-flow-tracing.ko.md)이
정의하는 `outcome=failed`와 `reason`으로, 원인 설명 문자열을 구현이 정한 길이
제한 안에서 실어 **그 실패를 만든 message와 같은 `flow` 아래** 기록한다.

이 기록을 켜고 끄는 비용 규칙은
[03. Message flow tracing 「5. 실행 중 기록 수준 변경과 비용 규칙」](03-message-flow-tracing.ko.md#5-실행-중-기록-수준-변경과-비용-규칙)이
정의한다.

## 5. 이 주제가 정의하지 않는 것

| 내용 | 소유 문서 |
|---|---|
| Host relocation·shutdown 개별 operation의 진행과 결과 | [Host relocation 전체 흐름](../05-location-relocation/05-host-relocation-flow.ko.md) |
| Application이 message와 함께 보내는 metadata의 소유권과 크기 | [Message model](../00-foundation/05-message-model.ko.md) |
| Transport 연결의 liveness와 peer deadline | [Transport connection liveness](../02-channel-transport/05-transport-liveness.ko.md) |
| STREAM 연결 close 사유(`close_reason`)의 정의 | [Session 「STREAM 서버 session」](../04-session/01-stream-session.ko.md) |

---

[스펙 목차](../README.ko.md) · [다음: 01. Runtime 상태와 운영 진단](01-runtime-monitoring.ko.md)
