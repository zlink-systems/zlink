---
title: "Framework 공통 E2E 인용과 표현 정합화 체크리스트"
---

# Framework 공통 E2E 인용과 표현 정합화 체크리스트

- **상태**: 공통 E2E 한영 문서 반영 완료. Runner·다섯 언어 process 증거는 별도 구현 gap이다.
- **작성일**: 2026-08-07
- **검토 기준**: `main`의 `09d34089c0956`
- **상위 결정**: [Framework 계약과 공통 E2E 정합성 결정 검토](framework-contract-e2e-decision-review.ko.md)
- **목적**: 결정 항목이 직접 바꾸는 scenario뿐 아니라 절 번호·anchor drift, 표현, 제목과 역참조,
  같은 계약을 사용하는 중복 scenario와 언어별 적용 범위를 공통 E2E 수정에서 함께 닫는다.

## 1. 실행 시점과 완료 조건

이 작업은 상위 결정 문서의 Phase 1에서 공통 spec heading과 닫힌 값이 확정된 뒤 실행한다.
그 전에 현재 절 번호만 고치면 timer, tracing과 STREAM timeout 계약을 반영한 뒤 같은 link를 다시
수정해야 할 수 있다.

각 항목은 다음 조건을 모두 만족해야 완료다.

1. 한국어 E2E의 설명과 link를 현행 spec의 의미를 기준으로 수정한다.
2. 영문 counterpart에 같은 의미와 link를 반영한다.
3. Scenario 제목이 바뀌면 생성된 anchor와 모든 역참조를 함께 바꾼다.
4. 언어별 feature map, E2E README의 완료 목록과 scenario index를 함께 갱신한다.
5. Link target 파일의 존재뿐 아니라 `#fragment`가 현재 Markdown renderer에서 실제로 생성되는지 검사한다.
6. 변경 뒤에도 scenario ID는 유지한다. ID를 바꿔야 한다면 모든 runner와 evidence key의 파급을 별도
   변경으로 검토한다.

2026-08-07 반영에서는 54개 citation의 의미 owner, DEC와 독립된 표현, 제목·역참조, cancellation의
언어별 적용 범위, SF-C5A와 timer tick 판정을 한국어·영문에 함께 적용했다. 첫 독립 리뷰에서 발견한
file-only·구판 절 인용 50개도 실제 semantic heading fragment로 교체했다. Scenario ID는 유지했으며
한영 ID 집합이 일치한다. Repository link·anchor 검사와 tab 검사는 통과했다. 아래 checkbox는 현재
문서 반영 상태를 나타내며 runtime·runner·process E2E 증거의 완료를 뜻하지 않는다.

독립 리뷰에서 추가로 발견한 RL-F7·ST-G2·ST-G4·ST-G6의 Actor/Spot-scoped Relocate 표현도 수정했다.
RL-F7은 public Actor Join으로 membership을 변경하고, 나머지는 target 인자 없는 public Host Relocate와
Framework target selection을 사용한다. C++·.NET·Node regression matrix와 다섯 언어 channel guide도
public observer가 아니라 application logger/telemetry provider의 structured record를 판정하도록 맞췄다.
후속 독립 Codex Sol 리뷰는 54개 semantic citation, 추가 scenario와 regression matrix 파급을 다시 확인하고
남은 P0/P1 문서 gap이 없다고 최종 승인했다.

제공된 검토 목록을 scenario ID로 펼치면 citation 재검증 대상은 54개다. 이 숫자는 link 개수가 아니라
scenario 개수다. 한 scenario가 둘 이상의 spec을 인용하면 모든 link를 각각 확인한다.

## 2. 절 번호와 anchor 재검증

표의 “최종 owner”는 숫자만 고치는 기준이 아니라 scenario가 실제로 검증하는 의미다. Phase 1에서
heading 번호가 바뀌더라도 이 owner를 찾아 최종 anchor를 생성한다.

### 2.1 Config 8 — execution turn

대상: [Config 8](../framework/common/e2e/config-8-execution-turn.ko.md)

| 완료 | Scenario | 최종 owner |
|---|---|---|
| [x] | TD-B3 | Async spec의 `Handler turn과 claim`과 `Yield 시 gate와 claim` |
| [x] | TD-C1, TD-C2, TD-C3, TD-C4, TD-C5 | Async spec의 `Worker offload`. DEC-03의 CPU execution slot과 I/O wait 불변 조건 포함 |
| [x] | TD-D1, TD-D2, TD-D4 | Async spec의 `Handler turn과 claim`과 lane별 gate 의미 |
| [x] | TD-D6 | Async spec의 `같은 turn에서의 대기`와 claim cycle 거부 |
| [x] | TD-E2A | Async spec의 `Actor Join의 deferred terminal` |
| [x] | TD-F3 | Session Actor spec의 execution·lifecycle과 async spec의 handler turn |
| [x] | TD-F5, TD-F5A | Async spec의 `Cancellation과 shutdown`; F5A는 graceful drain의 shutdown 경쟁도 함께 인용 |
| [x] | TD-F6 | Async spec의 `같은 turn에서의 대기`와 wait-for cycle 거부 |

구판 `§7`부터 `§10`까지의 번호를 현재 파일의 다른 번호로 기계적으로 치환하지 않는다. Scenario가
검증하는 gate, worker, cancellation과 deferred Join 의미를 각각 소유한 heading으로 나눈다.

### 2.2 Config 5 — resilience와 lifecycle

대상: [Config 5](../framework/common/e2e/config-5-resilience-lifecycle.ko.md)

| 완료 | Scenario | 기존 drift | 최종 owner |
|---|---|---|---|
| [x] | RL-B3, RL-C1 | Host maintenance `§10` | Graceful drain `Shutdown과 Relocate의 경쟁` |
| [x] | RL-F9 | Host maintenance `§12` 하나만 인용 | `Relocate 완료와 실패`와 `Shutdown과 Relocate의 경쟁` |
| [x] | RL-F1 | Host maintenance `§6` | `Mode에 맞는 target 선택`과 `Relocation unit과 실행량 제한` |
| [x] | RL-F5 | Location runtime `§8`, graceful drain `§8` | Location runtime `Target이 새 message를 받기 시작하는 시점`과 graceful drain의 unit handoff |
| [x] | RL-F12 | Spot actor `§8` | Graceful drain `대기 중인 message, timer와 session을 옮긴다` |
| [x] | RL-B5 | Channel topology `§5.1` | `Weight 변경은 연결을 다시 만들지 않는다` |

RL-F12는 `SpotWide` User Spot 또는 Instance Spot fixture만 사용한다. Entry Spot과 `PerActor` User Spot의
Spot-level application timer를 이전 성공 조건에 넣지 않는다.

### 2.3 Config 6 — Store failure와 recovery

대상: [Config 6](../framework/common/e2e/config-6-store-failure-recovery.ko.md)

| 완료 | Scenario | 최종 owner |
|---|---|---|
| [x] | SF-A1 | Location runtime의 정상 등록·조회 순서와 current record read |
| [x] | SF-B3 | Failure policy의 object owner lease와 Store 장애 경계. Host relocation 장애만 인용하지 않는다. |
| [x] | SF-C1 | Location runtime의 실행 중 node·capability 조회와 lease 만료 뒤 제외 |
| [x] | SF-D1, SF-D2 | Location runtime의 Store 단절 fence와 복구 뒤 current registration 수렴 |
| [x] | SF-E1 | Async spec의 `Worker offload`와 DEC-03의 I/O wait 격리 |
| [x] | SF-F3 | Relocation Store의 취소·오류 결과와 새 relocation admission |
| [x] | SF-F11 | Relocation Store의 취소·결과 재구성과 payload 게시·정리 |

`§2`, `§5`, `§6`처럼 현재 내용과 우연히 같은 번호가 있더라도 제목 의미가 다르면 통과시키지 않는다.
Spec 21·23·31의 최종 heading을 기준으로 anchor를 다시 만든다.

### 2.4 Config 2 — Spot, Session과 timer

대상: [Config 2](../framework/common/e2e/config-2-spot-service.ko.md)

| 완료 | Scenario | 최종 owner |
|---|---|---|
| [x] | SM-A2 | Spot messaging의 application queue·turn과 callback 순서 |
| [x] | SM-A6 | Spot actor의 membership·lifecycle과 Spot messaging의 close failure 경계 |
| [x] | SM-A8 | Async spec의 `Worker offload`와 handler turn |
| [x] | SM-D10 | STREAM recv·send admission과 async spec의 send deadline·backpressure |
| [x] | SM-D13 | Transport liveness의 heartbeat loss와 STREAM disconnect lifecycle |
| [x] | SM-D14 | STREAM session의 `TLS`, auth callback과 Actor binding |
| [x] | SM-E2 | Async spec의 `Spot timer`와 Spot application turn |
| [x] | SM-E3 | Async spec의 `Spot timer`와 Spot close 의미 |
| [x] | SM-E4 | DEC-04가 추가한 공통 timer overrun policy와 tick field |

존재하지 않는 STREAM session `§10`을 다른 숫자로 추측하여 바꾸지 않는다. TLS는 `TLS` heading,
heartbeat는 transport liveness, timer는 DEC-04 반영 뒤의 async spec을 직접 가리킨다.

### 2.5 Config 11 — observability와 host operation

대상: [Config 11](../framework/common/e2e/config-11-observability-ops.ko.md)

| 완료 | Scenario | 최종 owner |
|---|---|---|
| [x] | OBS-C2 | Graceful drain의 Actor handoff와 Session Actor route barrier |
| [x] | OBS-C4, OBS-C8 | Graceful drain의 shutdown 경쟁, deadline과 bounded teardown |
| [x] | OBS-C5, OBS-C9A, OBS-C9B | Graceful drain의 preflight와 target 선택 |
| [x] | OBS-C11 | Graceful drain의 concurrent 호출과 cancellation |
| [x] | OBS-B2 | Runtime metrics의 object·STREAM metric과 host relocation metric. 필요한 두 heading을 모두 인용 |
| [x] | OBS-A4 | Flow correlation의 생성·전파 규칙과 DEC-01의 fanout 기록 경계. Attribute 이름은 `flow_origin` 사용 |
| [x] | OBS-A5 | Message-flow tracing의 실행 중 level 변경 heading |

OBS-A4는 단순 attribute rename만으로 닫지 않는다. DEC-01 뒤에도 Classic fanout이 정상
`zlink.message_flow`를 만들지 않는다면 subscriber trace 공유 assertion을 제거하고, application payload의
flow propagation 또는 subscriber-local dispatch error 중 정식 계약이 허용한 결과로 다시 쓴다.

### 2.6 Config 13 — submit admission

대상: [Config 13](../framework/common/e2e/config-13-submit-admission.ko.md)

| 완료 | Scenario | 최종 owner |
|---|---|---|
| [x] | SA-E2E-06 | Graceful drain의 host state·완료 결과와 state별 admission |
| [x] | SA-E2E-15 | Async spec의 one-way submit·admission deadline과 Session Actor relay |

### 2.7 Config 1, 3과 14

| 완료 | Scenario | 최종 owner와 수정 범위 |
|---|---|---|
| [x] | RM-C8 | RouteMesh topology의 SS message 크기와 STREAM session의 `MaxMessageSize` owner를 연결한다. Spec 07 내부의 stale `#4-stream-socket-message-size` link도 같은 변경에서 고친다. |
| [x] | PS-E2A | Framework API의 automatic fanout Store prerequisite를 소유한 최종 heading으로 연결한다. |
| [x] | IS-E2E-17 | Framework API의 pending admission bound와 async spec의 object activation·backpressure heading을 함께 연결한다. |

## 3. DEC와 독립된 표현 수정

### 3.1 PS-F5 topic filter 표현

[PS-F5](../framework/common/e2e/config-3-pubsub.ko.md#ps-f5-구독하지-않은-traffic-중에도-liveness를-유지한다)의
“Subscriber는 `events.b`만 구독한다”는 문장을 사용하지 않는다. Public topic filter가 없으므로 다음
조건으로 바꾼다.

> Subscriber는 ChannelName에 연결되어 있고 `events.b` packet handler만 등록한다. Publisher는 handler가
> 없는 `events.a`를 계속 publish한다.

Scenario의 목적은 transport subscription filter가 아니라 handler 미등록 traffic 중 liveness 유지다.

### 3.2 IS-E2E-07과 IS-E2E-29 target 지정 표현

[IS-E2E-07](../framework/common/e2e/config-14-instance-spot.ko.md#is-e2e-07-normal-relocate)과
[IS-E2E-29](../framework/common/e2e/config-14-instance-spot.ko.md#is-e2e-29-cross-mesh-in-flight-relocate)의
“B로 Relocate” 절차를 다음 조건으로 바꾼다.

> B만 mode, version, capacity와 readiness 조건을 만족하는 eligible target인 topology를 구성한 뒤 public
> host Relocate를 시작한다.

Application은 B를 operation 인자로 지정하지 않는다. B에서 처리됐다는 evidence는 Framework target
selection 결과를 확인하는 데 사용한다.

### 3.3 OBS-B1 reconnect metric owner

[OBS-B1](../framework/common/e2e/config-11-observability-ops.ko.md#obs-b1-stream-connection과-reconnect-metric을-확인한다)은
server runtime의 active connection gauge와 connector의 reconnect counter를 분리한다. Server gauge는
Framework runtime metrics를 인용하고, 자동 reconnect의 계기·backoff·counter는
[Stream Connector reconnect 계기](../framework/common/spec/stream-connector/32-stream-connector.ko.md#62-connector-reconnect-계기)를
판정 기준으로 명시한다. Server runtime이 client reconnect를 시작한다고 설명하지 않는다.

## 4. Scenario 제목과 역참조

판정 수단이나 책임 주체가 바뀌면 본문만 고치지 않고 제목도 바꾼다. 최소 변경 대상은 다음과 같다.

| Scenario | 기존 제목의 문제 | 제목 방향 |
|---|---|---|
| SM-D9 | Public inbound observer가 남는다고 읽힌다. | Logger provider가 STREAM message-flow record를 받는 결과를 드러낸다. |
| RL-D2 | 제거할 observer가 실패 주체로 남는다. | Telemetry provider failure 격리를 드러낸다. |
| SM-D7 | Framework가 auth gate를 소유한다고 읽힌다. | Application session callback이 인증 전 packet을 거부하는 결과를 드러낸다. |

제목을 바꾼 뒤에는 다음 순서로 역참조를 갱신한다.

1. 상위 결정 문서의 scenario link
2. 한국어·영문 E2E counterpart 사이의 heading과 anchor
3. 다섯 언어 feature map의 scenario ID 행
4. E2E README, 완료 목록과 scenario index
5. Runner log와 evidence 문서에서 ID가 아닌 옛 제목이나 anchor를 직접 사용하는 곳

## 5. RL-F12 fixture 제한

[RL-F12](../framework/common/e2e/config-5-resilience-lifecycle.ko.md#rl-f12-user-spot-queue와-timer를-relocation-뒤-복원한다)의
시작 조건에 `SpotWide` User Spot 또는 Instance Spot을 명시한다. `PerActor` User Spot을 사용하는 variant는
Actor timer가 Actor queue와 함께 이동하는지 별도 판정할 수 있지만, Spot-level application timer 복원을
요구하지 않는다. Entry Spot의 Spot-level application timer도 relocation 성공 조건에서 제외한다.

## 6. 스펙 변경이 추가로 영향을 주는 scenario

### 6.1 Message-flow observer와 Framework-owned sink 제거

다음 scenario는 기존 DEC-01 직접 목록 밖에서도 public observer 또는 Framework-owned sink를
시작 조건으로 사용한다. Observer declaration을 제거하기 전에 application logger provider만으로
판정할 수 있는지 다섯 언어 process E2E에서 먼저 증명한다.

| 완료 | Scenario | 수정 방향 |
|---|---|---|
| [x] | SM-B5 | Public message-flow observer 등록을 제거하고 Actor `no_handler`의 `zlink.dispatch_error`와 정상 후속 request를 application logger provider로 확인한다. |
| [x] | SM-E1 | Public message-flow observer 등록을 제거하고 Spot `no_handler`의 `zlink.dispatch_error`와 정상 후속 request를 application logger provider로 확인한다. |
| [x] | RL-D3 | “Public logging sink”를 Framework public API로 설명하지 않는다. Application logger provider가 받은 정식 dispatch-error field와 정상 handler 격리를 확인한다. |

Config 3 완료 기준도 “status·observer·application evidence”를 “status·application logger
provider·application evidence”로 바꾼다. `MON-C1`의 topology/status observer와 `PS-D7A`의 fanout status
observer는 message-flow observer가 아니므로 제거하지 않는다.

### 6.2 Diagnostics level과 공통 E2E logging 설정

| 완료 | 대상 | 수정 방향 |
|---|---|---|
| [x] | OBS-A1 | `key_transitions`를 공통 level `Normal`로 바꾼다. |
| [x] | OBS-A3 | Source·target은 `Normal`, 중간 node는 `Off`로 적는다. |
| [x] | OBS-A5 | 전환을 `Normal` → `Off` → `Errors` → `Normal`로 적는다. |
| [x] | E2E README | `key_transitions`, `errors_only`와 C++ `diagnostics.log_file`을 제거한다. Application logger provider와 application이 구성한 file backend만 공통 evidence 경로로 사용한다. |

Tracing이나 logger provider failure는 application message operation의 결과를 바꾸지 않아야 한다.
README의 “observer/trace failure”도 공개 observer가 남는 것으로 읽히지 않도록 telemetry/logger provider
failure로 바꾼다.

### 6.3 ClientServer role 오류의 중복 scenario

[RL-F4](../framework/common/e2e/config-5-resilience-lifecycle.ko.md#rl-f4-client-role이-없는-clientserver-process는-outbound-호출하지-못한다)의
Server-only call 결과를 CH-E2E-05와 같은 `NotConfigured`로 바꾼다. ChannelName 또는 target 자체가
없는 경우에만 `NotFound`를 사용하고, local handler가 실행되지 않는 기존 검증은 유지한다.

### 6.4 Rebind callback과 tombstone

| 완료 | Scenario | 수정 방향 |
|---|---|---|
| [x] | SM-D4A | Rebind가 이전 exact binding에 disconnect callback을 최대 한 번 전달하고 callback terminal 뒤 tombstone을 남기는지 확인한다. Callback failure도 이전 binding을 복원하거나 새 binding을 제거하지 않아야 한다. |
| [x] | SM-D4B | 같은 `ObjectGeneration`의 relocation route 갱신은 rebind가 아니므로 disconnect callback이 실행되지 않는지 확인한다. |
| [x] | RL-F2 | 일반 `unbind` API가 있는 것처럼 쓰지 않는다. 이전 binding identity에 대한 지연된 logical disconnect가 stale result로 끝나고 새 binding과 Actor state를 바꾸지 않는지 확인한다. |

### 6.5 Cancellation variant의 언어별 적용 범위

Cancellation은 다섯 언어가 같은 인자로 제공하는 Framework public API가 아니다. `.NET`의
`CancellationToken`, Node.js의 `AbortSignal`, Java의 returned stage와 Kotlin coroutine cancellation은
각 exact interface가 정의한 operation에만 적용한다. C++에 별도 public cancellation input이 없는
operation은 cancellation variant를 실행하지 않으며 새 API를 추가하지 않는다.

| 완료 | Scenario | 공통 수정 기준 |
|---|---|---|
| [x] | RL-B1, RL-E4 | Waiter cancellation 또는 request race는 exact interface가 지원하는 언어만 실행한다. 나머지 언어는 timeout·connection loss variant로 terminal-once와 late reply 격리를 검증한다. |
| [x] | SF-F11 | Relocation waiter cancellation은 지원 언어 variant로 분류한다. 공통 필수 결과는 Store response loss 뒤 payload 격리와 새 relocation의 checksum 보존이다. |
| [x] | TD-E2A, TD-F5 | Handler·waiter cancellation은 해당 callback/call에 cancellation 표면이 있는 언어만 실행한다. Exception, deadline과 shutdown variant는 공통 불변 조건을 유지한다. |
| [x] | OBS-C12 | Joined waiter cancellation을 모든 언어의 P0 입력으로 요구하지 않는다. 지원 언어는 waiter-only cancellation을, 나머지는 concurrent call과 Shutdown의 terminal-once를 검증한다. |
| [x] | SA-E2E-07, SA-E2E-19 | One-way cancellation은 해당 언어의 public admission cancellation이 있는 경우만 실행한다. Timeout·Shutdown 뒤 no-replay는 모든 언어에서 유지한다. |
| [x] | SA-E2E-17 | DEC-09의 STREAM send call별 timeout을 reply call에 적용하지 않는다. Reply token one-shot은 normal·socket send timeout·shutdown으로 공통 검증하고 cancellation은 지원 언어의 추가 variant로 둔다. |

Config 5·8·11·13의 확인 범위와 완료 기준도 cancellation이 모든 언어의 필수 public input으로 읽히지
않도록 고친다. “언어별 표현”만 적지 말고 scenario별 필수 variant와 지원 언어 추가 variant를 구분한다.

### 6.6 Object query 상태와 timer tick 정보

SF-C5는 ready object pagination, SF-F6는 concurrent page cycle consistency를 계속 소유한다. 두 scenario에
상태 전이를 섞지 않고 SF-C5A를 추가해 다음 결과를 검증한다.

- Missing ID의 exact lookup은 empty이고 page에는 항목이 없다.
- Factory gate에서 보류한 object는 exact lookup과 page에서 `Creating`이다.
- 생성이 끝난 object는 `Ready`다.
- Commit 뒤 owner를 사용할 수 없으면 `Unavailable` entry를 반환한다.
- Store 조회 실패는 `Unavailable` Framework error로 끝나며 일부 page를 성공으로 반환하지 않는다.

SM-E4는 callback count와 spacing만 비교하지 않는다. Policy별 `DeliveryIndex`, `ScheduledIndex`와
`SkippedTicks`를 application evidence에 기록하고, `CatchUpBounded`는 configured bound를 넘는 callback을
만들지 않는지 확인한다. Exact scheduler nanosecond는 성공 조건으로 사용하지 않는다.

### 6.7 Actor·Spot 단위 Relocate 표현과 regression matrix 파급

| 완료 | 대상 | 수정 방향 |
|---|---|---|
| [x] | RL-F7 | 공개 Actor Relocate를 전제하지 않고 public Actor Join으로 target Spot membership을 변경한다. |
| [x] | ST-G2, ST-G4, ST-G6 | Spot 단위 Relocate를 전제하지 않고 target 인자 없는 public Host Relocate를 사용한다. Target은 Framework가 선택한다. |
| [x] | C++·.NET·Node regression matrix | Public observer/event 통과 기준을 제거하고 application logger/telemetry provider의 structured record와 provider-failure 격리를 검증한다. |
| [x] | 다섯 언어 channel guide | `message flow 로그/observer` 표현을 제거하고 application logger/telemetry provider 경계로 통일한다. |

## 7. 실행 순서

1. Phase 1 뒤 spec heading과 닫힌 attribute 값을 확정한다.
2. §2의 54개 scenario citation을 의미 owner 기준으로 resolve한다.
3. §3의 표현 세 건과 §5의 fixture 제한을 한국어·영문에 반영한다.
4. §6.1부터 §6.4까지의 observer, diagnostics, role error와 rebind 의미를 반영한다.
5. §6.5의 cancellation applicability를 scenario와 config 완료 기준에 반영한다.
6. SF-C5A를 추가하고 SM-E4의 tick field 판정을 보완한다.
7. §6.7의 Actor·Spot 단위 Relocate 표현과 regression matrix·guide 파급을 반영한다.
8. §4의 제목 세 건을 바꾸고 모든 역참조를 갱신한다.
9. 모든 Markdown link와 anchor를 site와 같은 Unicode slug 규칙으로 검사한다.
10. E2E scenario ID, feature map, README 완료 목록과 runner selection이 그대로 대응하는지 확인한다.
11. 체크박스가 모두 닫히고 변경 전후 link audit 결과가 남아야 상위 결정 문서 Phase 4를 완료한다.
