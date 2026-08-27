# spec/server 재구성 — observability `01-runtime-monitoring` · `02-runtime-metrics` 대장

> [매핑표](mapping.ko.md) §5의 규칙 등가성 대장에서 `24-runtime-monitoring` 유래
> (R1–R34)와 `25-runtime-metrics` 유래(R35–R50), 그리고 두 문서의 검증 요구 절에만
> 있던 독립 규칙(R91, R92)의 "새 위치" 열을 채운 결과다. R51–R99(message-flow-tracing·
> flow-correlation·README 디버깅 원칙 유래)는 이 문서의 범위가 아니다.

산출물: [`01-runtime-monitoring.ko.md`](../../../../../framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md)(430줄),
[`02-runtime-metrics.ko.md`](../../../../../framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md)(366줄).

## `24-runtime-monitoring` 유래 (R1–R34)

| R# | 새 위치 | 비고 |
|---|---|---|
| R1 | 01 §1 | |
| R2 | 01 §2 | |
| R3 | 01 §2 | |
| R4 | 01 §3 | |
| R5 | 01 §3 | |
| R6 | 01 §3 (비규범적 C# 발췌) | |
| R7 | 01 §3 | |
| R8 | 01 §3 | |
| R9 | 01 §3 (`SafeToShutdown`) | 굵은 규칙 문장 + 이유 불릿, "결정" 라벨 없음 |
| R10 | 01 §4 | |
| R11 | 01 §4 (Core HWM snapshot 필드) | |
| R12 | 01 §4 (Application job queue snapshot 필드 + Reset) | Reset·epoch 소유는 02 §3을 링크로 명시(S6) |
| R13 | 01 §4 | |
| R14 | 01 §5 | |
| R15 | 01 §5 | |
| R16 | 01 §5 | |
| R17 | 01 §5 | |
| R18 | 01 §5 | |
| R19 | 01 §5 | |
| R20 | 01 §6 | |
| R21 | 01 §6 | |
| R22 | 01 §7.1 (Source의 정의) | S4: 번호 부여 |
| R23 | 01 §7.1 | |
| R24 | 01 §7.2 (합치기) | S4: 번호 부여 |
| R25 | 01 §7.2 | |
| R26 | 01 §7.2 | |
| R27 | 01 §8 | |
| R28 | 01 §9 | |
| R29 | 01 §9 (structured log identifier 12개 표) | |
| R30 | 01 §9 | |
| R31 | 01 §9 | |
| R32 | 01 §9 | |
| R33 | 01 §9 | |
| R34 | 01 §10 | |

## `25-runtime-metrics` 유래 (R35–R50)

| R# | 새 위치 | 비고 |
|---|---|---|
| R35 | 02 §1 | |
| R36 | 02 §1 | |
| R37 | 02 §2 | |
| R38 | 02 §3 (계기 14개 표) | |
| R39 | 02 §3 (Reset) | S6: epoch·Reset 의미를 이 문서가 소유한다고 명시, 01 §4가 링크 |
| R40 | 02 §3 | |
| R41 | 02 §4 (계기 8개 + label 표) | 계기 8개: `peers.configured`/`peers.connected`/`peers.ready`/`channels.ready_members`/`channel.selection_failures`/`requests.inflight`/`request.duration`/`request.timeouts` |
| R42 | 02 §5 | |
| R43 | 02 §6 | |
| R44 | 02 §6 (계기 + label 표) | |
| R45 | 02 §7 | S7: 25 §4에서 분리 |
| R46 | 02 §8 | |
| R47 | 02 §8 (3구간 분리, node-local clock) | "결정" 라벨 없이 굵은 규칙 문장; S15: source/target 2-participant sequence diagram 추가 |
| R48 | 02 §9 | `operation` 7값: `read`/`compare_exchange`/`relocation_put`/`relocation_get`/`relocation_delete`/`lease_renew`/`release` |
| R49 | 02 §10 | |
| R50 | 02 §11 | "소급" → "지나간 처리 단계에 되돌려 적용"으로 교체(S16) |

## 검증 요구 절 전용 독립 규칙

| R# | 새 위치 | 비고 |
|---|---|---|
| R91 | 01 §11 (검증 요구, "위치 조회와 log") | 24 §7에만 있던 독립 금지 규칙 |
| R92 | 02 §12 (검증 요구, "Relocation과 host lifecycle") | 25 §9에만 있던 교차 문서 정합 요구 |

빈 "새 위치" 행 0건, 대장에 없는 추가 보장 0건(재작성 중 원문에 없는 이름·수치·방향
문장을 새로 만들지 않았다).

## 닫힌집합 크기 확인

| 집합 | 매핑표 기재 크기 | 본문 확인 결과 |
|---|---|---|
| Peer/channel metrics(02 §4) | 8 | `peers.configured`/`peers.connected`/`peers.ready`/`channels.ready_members`/`channel.selection_failures`/`requests.inflight`/`request.duration`/`request.timeouts` — 8개, 원문(25 §3.2)과 동일 |
| `operation`(02 §9, Location store) | 7 | `read`/`compare_exchange`/`relocation_put`/`relocation_get`/`relocation_delete`/`lease_renew`/`release` — 7개, 원문(25 §6)과 동일 |
| structured log 22 key | (03-message-flow-tracing 소유, 이 문서의 범위 아님) | 확인하지 않음 — R61 등은 26 유래이며 이 대장의 R1–R50에 없다 |

## 이동 후 갱신할 링크

`spec/server/`가 옛 경로 그대로 있는 동안 다음 링크는 **아직 이동하지 않은 문서**를
가리키는 옛 경로다. 해당 문서가 각자 주제로 이동할 때 상대 경로와 anchor를 함께
갱신해야 한다.

| 링크 | 사용한 곳 | 비고 |
|---|---|---|
| `../01-glossary.ko.md#*` (전체) | 01, 02 다수 | glossary는 `00-foundation`으로 이동 예정(옛 `01-glossary` → 새 `02-glossary`, target-readme.ko.md 기준). anchor는 대부분 원문과 동일한 문자열을 유지했으므로 파일명 치환만 필요 |
| `../30-host-relocation-flow.ko.md` | 01 §1·§3, 02 §1·§8·§12 | `05-location-relocation` 주제로 이동 예정 |
| `../28-relocation-flow.ko.md` | 01 §3 | `05-location-relocation` 주제로 이동 예정 |
| `../21-location-runtime.ko.md#64-운영-도구에서-현재-위치를-조회한다` | 01 §8 | `03-spot-actor` 또는 `05-location-relocation` 주제로 이동 예정(topic-map 확인 필요) |
| `../06-framework-api.ko.md` | 02 §8 | `00-foundation` 또는 `01-execution` 주제로 이동 예정 |
| `../languages/dotnet/interfaces/10-topology-monitoring.ko.md` | 01 §3 | 언어별 문서 트리는 `languages/`에 그대로 둔다(README §2) — 경로 접두사만 확인 필요 |

## 교차 주제 확인 필요

mapping.ko.md §4 S10–S13에서 넘어온 항목. 이번 재작성에서는 해당 문서의 서술을
그대로 옮기고 판정하지 않았다. 소유 문서가 재작성될 때 이 문서의 링크·값이 맞는지
재확인한다.

| # | 내용 | 이 문서에서의 서술 | 소유(추정) 문서 |
|---|---|---|---|
| S10 | 01 §5의 automatic fanout 15초 record timeout이 `29-transport-liveness`(channel-transport 주제)가 소유하는 15초 peer deadline과 같은 상수 계열인지 이 문서 안에서 밝히지 않음 | 원문 그대로 "15초 동안 record가 없으면" 서술만 유지 | `02-channel-transport/05-transport-liveness.ko.md` |
| S11 | 02 §6 label 표의 `close_reason` 닫힌집합(`client_close`…`transport_error`)의 실제 정의는 `04-session/01-stream-session.ko.md`가 인용하는 stream-connector 스펙 §6.3(다른 spec 트리)이 소유 | 값 목록만 표로 유지, 정의 문서는 밝히지 않음(원문 그대로) | `stream-connector/32-stream-connector.ko.md#63-종료-사유` |
| S12 | 01 §3의 `SafeToShutdown` 판정 조건(Message Follow route 제거 가능 시점, cutover 재전송 창 종료)의 정의는 `05-location-relocation`(28·30)이 소유 | "정의는 [Actor와 Spot relocation 전체 흐름](../../../../framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md)이 소유한다"로 링크만 유지 | `28-relocation-flow.ko.md` |
| S13 | 02 §8의 relocation reason·outcome 식별자는 `30-host-relocation-flow`(location-relocation 주제)가 소유("Reason은 30의 식별자를 사용한다") — 재작성 순서상 이 주제가 먼저 끝나면 30 쪽 문서명이 바뀔 수 있음 | "Reason은 [Host relocation과 shutdown](../../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md)의 식별자를 사용한다"로 링크만 유지 | `30-host-relocation-flow.ko.md` |

## spec-gap 후보

이번 재작성 범위(R1–R50, R91–R92)에서 새로 발견한 gap은 없다. mapping.ko.md §"spec-gap
후보"의 G1·G2가 이 두 문서에 걸친 기존 후보다 — 재작성 과정에서 재확인했고 내용을
바꾸지 않았다.

| # | 내용 | 이 문서에서 관찰되는 위치 |
|---|---|---|
| G1 | `02-runtime-metrics` §11("Metric을 끈 경로는…")과 `01-runtime-monitoring` §10("Metric이나 trace를 끄더라도…")이 "metric을 끈다"는 전제를 두 번 쓰지만, metric 전용 on/off 설정(이름·기본값·범위)이 두 문서 어디에도 정의되어 있지 않다. `03-message-flow-tracing`(26 유래) §4는 diagnostics level이 message-flow trace 전용이며 "diagnostics level은 metric 기록을 끄지 않는다"고 명시하므로, metric을 끄는 것이 diagnostics level과 별개의 설정이라는 것만 알 수 있고 그 설정 자체는 스펙에 없다. | `02-runtime-metrics.ko.md` §11, `01-runtime-monitoring.ko.md` §10 |
| G2 | `01-runtime-monitoring` §4의 Core HWM snapshot 필드 목록(문단)에 있는 "monitor queue applied/accounted", "total instance applied/accounted bytes", "active ordinary/completion/send/receive queue 수"에 대응하는 지속 계기가 `02-runtime-metrics` §3의 계기 표에 없다(§3은 `effective_budget`/`applied`/`accounted`/`completion_accounted`/`blocked_ratio` 5개만 계기로 노출). 1회 조회(status)에서만 보이고 시계열 metric으로는 의도적으로 제외한 것인지, 정의가 빠진 것인지 스펙에 근거가 없다. | `01-runtime-monitoring.ko.md` §4, `02-runtime-metrics.ko.md` §3 |
