# spec/server 재구성 — observability 주제 매핑표

> 캠페인: `framework/doc/framework/common/spec/server`를 주제별로 재구성하고
> [스펙 문서 작성 가이드](../../../../principal/documentation/spec-writing-guide.ko.md)대로 다시 쓴다.
> 이 문서는 여섯째 주제 `observability`의 작업 계획이다. 양식은
> [04-session 파일럿 매핑표](../04-session/mapping.ko.md)를 따른다.

관련: [spec-gap 대장](../../spec-gap.ko.md) · [주제 구분 초안](../../topic-map.ko.md) ·
[전체 목차 초안 — 06-observability](../../target-readme.ko.md)

## 1. 대상과 규모

| 현재 문서 | 줄 수 | 성격 | 외부 anchor 링크 수 |
|---|---|---|---|
| `24-runtime-monitoring` | 391 | 계약 — 현재 status 조회, 변화 관찰, structured log identifier | 9 |
| `25-runtime-metrics` | 299 | 계약 — metric 이름·종류·단위·label과 집계 규칙 | 7 |
| `26-message-flow-tracing` | 308 | 계약 — message 한 건의 처리 단계 trace, attribute, 기록 수준 | 13 |
| `27-flow-correlation` | 210 | 계약 — `correlation_id`·`flow_id`·`flow_origin`의 생성·전파·수명 | 11 |

앞의 "외부 anchor 링크 수"는 다른 문서가 `#절-anchor`까지 지정해 해당 문서를 참조하는
occurrence 수다(같은 문서 안의 자기 참조는 제외). 이 중 **서로 다른 anchor** 수는 각각
4/2/5/5(합 16종)다 — anchor 치환표는 이 16종을 채우면 된다. 네 문서를 경로만으로라도
참조하는 외부 `.md` 파일은 총 **133개**(언어별 guide·e2e·sample·internal·reference·
contract-inventory 문서, 4개 문서 사이에 중복 포함) — session 파일럿의 87개보다 넓게
퍼져 있다. 이는 네 문서가 각 언어 `guide/server/04-backpressure`·`11-monitoring`·
`12-operations` 3종 문서 모두에서 반복 인용되기 때문이다.

**코드에서 이 문서를 경로로 여는 곳은 없다.** cpp/`.cs`/`.java`/`.kt`/`.ts`/`.py`/`.sh`
전체를 검색해도 `24-runtime-monitoring`·`25-runtime-metrics`·`26-message-flow-tracing`·
`27-flow-correlation` 문자열이 나오지 않는다. session 파일럿의 cpp layout contract test
같은 needle 갱신 작업은 이 주제에 없다. 단, `.json` 문서 3개
(`framework/doc/contract-inventory/route-mesh-v11-core-service-migration-inventory.json`,
`route-mesh-v11-public-contract-trace.json`, `route-mesh-v11-public-contract-trace.config.json`)가
네 문서를 경로 문자열로 담고 있다 — 코드가 열지는 않지만 이동 시 경로 치환 대상이다(§6).

**추가 입력 — 최상위 README의 두 절.** [주제 구분 초안](../../topic-map.ko.md) 마지막
문단이 지정한 대로, `framework/doc/framework/common/spec/server/README.ko.md`의
"디버깅 원칙"(약 25줄)과 "Trace 비용 규칙"(약 20줄) 두 절도 이 주제로 옮긴다. 두 절은
"대상 문서" 표에 들어가지 않지만 §3·§5에서 함께 다룬다. 원본 위치는
`README.ko.md:227-273`(디버깅 원칙)·`README.ko.md:264-273`(Trace 비용 규칙, 디버깅
원칙 절 안에 있음)이다.

## 2. 독자 질문 — 주제 README가 답할 것

가이드 §1의 질문표를 observability 주제에 맞게 채운 것. 새 문서의 절은 이 질문 순서를
따른다.

| 질문 | 답이 있어야 할 자리 |
|---|---|
| 운영자는 process 전체가 지금 새 작업을 받을 수 있는지 한 번에 어떻게 확인하는가 | README 개요 + `runtime-monitoring` Host 상태 |
| RouteMesh·ClientServer·automatic fanout 각각의 준비 상태는 무엇으로 확인하는가 | `runtime-monitoring` Topology 상태 |
| 상태가 바뀌는 순간을 놓치지 않으려면 무엇을 관찰하는가, 관찰자가 느리면 어떻게 되는가 | `runtime-monitoring` 변화 관찰과 합치기 |
| 지금 이 Actor·Spot이 어디 있는지 운영 도구로 조회하려면 | `runtime-monitoring` Object 위치 조회 |
| 상태가 바뀐 이유는 어디서 찾는가 | `runtime-monitoring` Structured log |
| 처리량·대기·실패·현재 개수를 dashboard로 보려면 어떤 이름의 수치를 모으는가 | `runtime-metrics` |
| 같은 계기를 모든 언어의 같은 dashboard·alert로 볼 수 있는 근거는 무엇인가 | `runtime-metrics` 이름과 집계 규칙, Label cardinality |
| Relocation 한 건이 얼마나 걸렸고 어디서 막혔는지 어떤 수치로 보는가 | `runtime-metrics` Host relocation과 shutdown |
| Message 한 건이 어느 처리 단계까지 갔고 어디서 실패했는지 어떻게 추적하는가 | `message-flow-tracing` |
| 이 흐름 기록을 켜고 끄는 비용은 얼마인가, 꺼도 정말 비용이 0인가 | `message-flow-tracing` 실행 중 기록 수준 변경 |
| Request와 그 reply는 무엇으로 연결되는가 | `flow-correlation` 두 식별자의 역할 |
| 여러 message가 같은 원인에서 시작됐다는 것은 무엇으로 아는가 | `flow-correlation` |
| 이 식별자들에 개인정보나 payload가 들어가는가 | `flow-correlation` 관측과 privacy |
| 간헐 실패를 조사할 때 운영자는 무엇부터 켜서 읽는가 | README 진단 순서(디버깅 원칙 이관) |

## 3. 새 구조

```
spec/server/06-observability/
  README.ko.md                   주제 진입 1장 — 개요, 책임 지도, 질문→문서 표, 진단 순서
  01-runtime-monitoring.ko.md    24 재작성
  02-runtime-metrics.ko.md       25 재작성
  03-message-flow-tracing.ko.md  26 재작성 (+ README "Trace 비용 규칙" 병합)
  04-flow-correlation.ko.md      27 재작성
```

파일명은 [target-readme.ko.md](../../target-readme.ko.md)의 초안과 같다(en 짝 문서 동일).

### 3.0 README 책임 지도 — 4개 서문의 반복을 한 곳으로

네 문서 모두 서문에서 "이 문서는 X를 소유하고, Y·Z는 다른 문서가 소유한다"를 서로 다른
문장으로 반복한다(§4 S5). 새 README는 이를 한 표로 둔다.

| 주제 안 문서 | 소유 범위 |
|---|---|
| `runtime-monitoring` | 특정 시점의 완전한 status, status 변화 stream, structured log identifier |
| `runtime-metrics` | 시간에 따라 누적·수집하는 metric의 이름·종류·단위·label |
| `message-flow-tracing` | message 한 건의 진행 기록(trace)과 그 attribute·기록 수준 |
| `flow-correlation` | `correlation_id`·`flow_id`·`flow_origin`의 생성·형식·전파·소유권·수명 |

각 문서 서문은 이 표를 링크하고 자기 소유 범위 한 문장만 남긴다. `30-host-relocation-flow`
(개별 host operation 결과, location-relocation 주제 소유)로 가는 링크도 표 아래 한 줄로
남긴다.

### 3.1 `01-runtime-monitoring` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Runtime 상태 조회 개요 — 범위, 소유 경계 | 24 §1(서문+책임표), §3.0 링크로 축소 | 계약 |
| 2. 역할과 책임 · public 미노출 값 | 24 §1 후반(책임표, descriptor revision·owner lease·미노출 목록) | 계약 |
| 3. Host 상태 — 한 번에 읽는 값 | 24 §2(서문, 표, C# 발췌), §2.1(runtime state 7값, IsReady/AcceptingWork, SafeToShutdown) | 계약 + **결정**(SafeToShutdown 게시 조건) |
| 4. Host status의 capacity 항목 | 24 §2.1(Core HWM·job queue snapshot 문단) → 필드-계기 대응은 `runtime-metrics` §3.1 표로 위임, 여기서는 "무엇을 관찰할 수 있는가"만 | 계약(축약 + 링크, S3) |
| 5. Topology 상태 — RouteMesh·ClientServer·automatic fanout | 24 §2.2 전체 | 계약 |
| 6. 상태 변화를 관찰한다 — Sequence와 완전한 status | 24 §3(서문, C# 발췌, Sequence 단조 증가) | 계약 |
| 7. 관찰자가 느릴 때 — source, 합치기, 유실 누계 | 24 §3 "Source의 정의"·"합치기"(번호 부여, S4) | 계약 + **결정**(source별 최신 status 한 자리) |
| 8. Object의 현재 위치 조회 | 24 §4 | 계약(링크로 21 §6.4 위임) |
| 9. Structured log | 24 §5 | 계약 |
| 10. Startup과 실패 | 24 §6 | 계약 |
| 11. 검증 요구 | 24 §7 | 검증 |

### 3.2 `02-runtime-metrics` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Metric 계약 개요 | 25 §1(§3.0 링크로 축소) | 계약 |
| 2. 이름과 집계 규칙 | 25 §2 | 계약 |
| 3. Host Core HWM과 Application job queue | 25 §3.1(계기 표, Reset 규칙, epoch 소유 명시 — S6) | 계약 |
| 4. Peer와 channel | 25 §3.2 | 계약 |
| 5. One-way message drop | 25 §3.3 | 계약 |
| 6. Object 수·capacity와 relocation 계기 | 25 §4 전반(spot/actor count, capacity, relocation.*, stream.connections.*) | 계약(질문 단위 분리, S7) |
| 7. Instance Spot activation 계기 | 25 §4 후반(activations, claim.conflicts, takeovers) | 계약 |
| 8. Host relocation과 shutdown | 25 §5 | 계약 + **결정**(3구간 분리, node-local clock만) |
| 9. Location과 telemetry | 25 §6 | 계약 |
| 10. Label cardinality | 25 §7 | 계약 |
| 11. 수집 경계 | 25 §8 | 계약 |
| 12. 검증 요구 | 25 §9 | 검증 |

### 3.3 `03-message-flow-tracing` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. 무엇을 확인할 수 있는가 | 26 §1(§3.0 링크로 축소) | 계약 |
| 2. 처리 단계 | 26 §2, §2.1(phase 9값 + diagram), §2.2(기록 대상) | 계약 |
| 3. 공통 attribute | 26 §3, §3.1(닫힌 값), §3.2(포함 조건 + structured log 대체 표기를 별도 소제목으로 분리, S8) | 계약 |
| 4. 기록 범위 설정 — level과 sampling | 26 §4 | 계약 |
| 5. 실행 중 기록 수준 변경과 비용 규칙 | 26 §4.1 + README "Trace 비용 규칙" 병합(S9) | 계약 + **결정**(Off는 read+branch 외 비용 없음) |
| 6. 완료, 실패와 수명 | 26 §5 | 계약 |
| 7. 검증 요구 | 26 §6 | 검증 |

### 3.4 `04-flow-correlation` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. 무엇을 식별하는가 | 27 §1(§3.0 링크로 축소) | 계약 |
| 2. 두 식별자의 역할 | 27 §2 | 계약 |
| 3. 형식과 소유권 | 27 §3 | 계약 |
| 4. Flow를 만드는 시점 | 27 §4 | 계약 |
| 5. 전파 규칙 | 27 §5(대상 public 동작 목록은 26 §2.2를 표준으로 링크, S14) | 계약 |
| 6. Async 작업과 execution context | 27 §6 | 계약 |
| 7. Reply와 실패 | 27 §7 | 계약 |
| 8. 관측과 privacy | 27 §8 | 계약 |
| 9. 검증 요구 | 27 §9 | 검증 |

### 3.5 새 README의 "진단 순서" (디버깅 원칙 이관)

옛 README "디버깅 원칙"의 1~3번(무엇을 먼저 켜는가, 어떻게 읽는가, 실패는 반드시
flow에 남긴다)은 운영 절차이지 개별 문서 하나의 계약이 아니므로 주제 README에
그대로 둔다. 4번(Trace 비용 규칙)만 `message-flow-tracing` §5로 옮기고(§3.3, S9)
README에는 그 절로 가는 한 줄 링크만 남긴다.

## 4. 읽으면서 발견한 구조 문제 (재작성에서 처리)

| # | 문제 | 처리 |
|---|---|---|
| S1 | 24 §1과 25 §1의 절 제목 "이 문서가 정의하는 계약"은 메타 제목(가이드 §2.2가 금지) | 주제 이름으로 교체 — "Runtime 상태 조회 개요" / "Metric 계약 개요" |
| S2 | Host runtime state 닫힌집합 7값(`preparing`…`error`)을 24 §2.1이 표로 정의하는데 25 §5가 `state`는 `preparing\|serving\|…\|error`다"로 같은 값을 문장으로 다시 나열 | `runtime-metrics` §8은 24 §3의 표를 링크만 하고 값 재나열 생략 |
| S3 | 24 §2.1의 Core HWM·Application job queue snapshot 필드가 15개 안팎을 한 문단에 나열하는데, `runtime-metrics` §3.1의 계기 표와 필드 집합이 겹치면서도 표기(문단 명사구 vs 계기 이름)가 달라 두 서술이 따로 어긋날 위험이 있다 | `runtime-monitoring` §4는 "이 status에서 무엇을 관찰할 수 있는가"만 짧게 쓰고 필드 하나하나의 대응은 `runtime-metrics` §3 표를 단일 출처로 링크(개별 필드는 spec-gap 후보 G2로 별도 처리) |
| S4 | 24 §3의 "Source의 정의"·"합치기" 소제목이 번호 없이(`###`만) 붙어 있어 절 번호 체계가 끊김 | `runtime-monitoring` §7 아래 §7.1·§7.2로 번호 부여 |
| S5 | 4개 문서 서문이 형제 문서의 소유 범위를 거의 같은 문장으로 4번 반복 서술 | §3.0 "책임 지도" 표로 통합, 각 서문은 그 표를 링크 |
| S6 | "measurement epoch" 개념을 24 §2.1이 먼저 쓰지만 정의(Reset 의미)는 25 §3.1에만 있어 소유 문서가 불명확 | `runtime-metrics` §3이 epoch·Reset 의미를 소유한다고 명시, `runtime-monitoring` §4는 그 절을 링크 |
| S7 | 25 §4 "Object와 STREAM" 한 절에 Spot/Actor count, capacity, relocation counters, STREAM 연결, label 표, Instance Spot activation까지 몰려 200줄대 벽 | 질문 단위로 §6(수·capacity·relocation)·§7(Instance Spot activation) 분리 |
| S8 | 26 §3.2 attribute 표 바로 뒤에 소제목 없이 "structured log 대체 표기(`zlink flow:` prefix)" 단락이 이어져 두 번째 표기 체계가 눈에 안 띔 | 자기 소제목으로 분리 |
| S9 | 최상위 README "Trace 비용 규칙"이 26 §4.1과 사실상 같은 zero-cost 요구를 다른 표현(`if(enabled(outcome))`, lazy trace)으로 다시 적어, 두 서술이 어긋나면 어느 쪽이 계약인지 불분명 | `message-flow-tracing` §5가 유일 소유. 두 서술을 합쳐 하나로 쓰고, README에는 이 절로 가는 링크만 남긴다 |
| S10 | 24 §2.2·§5의 automatic fanout 15초 record timeout이 `29-transport-liveness`(다른 주제, channel-transport)가 소유하는 15초 peer deadline과 같은 상수 계열인지 이 문서 안에서 밝히지 않음 | 교차 주제 확인 필요 — 이번 재작성에서는 손대지 않고 §6 교차 주제 표로 남긴다 |
| S11 | 25 §4 label 표의 `close_reason` 닫힌집합(`client_close`…`transport_error`)의 실제 정의는 `04-session/01-stream-session.ko.md`가 인용하는 stream-connector 스펙 §6.3(다른 spec 트리)이 소유 | 교차 주제 확인 필요 — §6에 남긴다 |
| S12 | `runtime-monitoring`의 `SafeToShutdown` 판정 조건(Message Follow route 제거 가능 시점, cutover 재전송 창 종료)의 정의는 `05-location-relocation`(28·30)이 소유 | 교차 주제 확인 필요 — §6에 남긴다 |
| S13 | `runtime-metrics` §5의 relocation reason·outcome 식별자는 `30-host-relocation-flow`(location-relocation 주제)가 소유("Reason은 30의 식별자를 사용한다") — 재작성 순서상 이 주제가 먼저 끝나면 30 쪽 문서명이 바뀔 수 있음 | 교차 주제 확인 필요 — §6에 남긴다 |
| S14 | 27 §5 "Instance Spot direct" 보존 범위 서술이 26 §2.2의 "기록하는 public 동작" 목록과 문장 단위로 거의 동일하게 반복 | 26 §2.2를 표준 목록으로 삼고 27 §5는 "같은 경계, flow 보존 범위는"으로 링크 참조 |
| S15 | 가이드 §7.2 diagram 조건(물리/논리 경계를 넘는 것) — 25 §5의 relocation 구간 3분할(source 정지 구간·target 재개 구간·route 수렴 구간)은 source·target 두 node를 가로지르며 "서로 다른 node의 시각을 직접 빼는 지표는 만들지 않는다"는 문장으로만 설명되어, 세 구간이 어느 node 시각 기준인지 문장만으로는 놓치기 쉬움 | `runtime-metrics` §8(Host relocation과 shutdown)에 source/target 2-participant sequence diagram 추가 — 세 구간의 시작·끝 이벤트를 각 node timeline 위에 표시 |
| S16 | 어휘 — 25 §8("Provider callback failure는 마지막 정상 수집 결과를 소급해서 바꾸지 않는다")과 27 §4·§9(outbound frame에 "변경을 소급 적용하지 않는다", 다시 켠 뒤 "이전 단계를 소급하여 기록하지 않는지")의 "소급"은 널리 안 쓰는 한자어(원칙 7.3). 세 문서 세 곳에 흩어진 같은 규칙(꺼졌다 켜져도 과거 처리 단계를 되돌려 기록하지 않음)이기도 하다 | "소급" → "지나간 처리 단계에 되돌려 적용"으로 교체; 세 문장이 같은 규칙임을 검증 요구 절 인접 항목으로 묶어 서술 |

## 5. 규칙 등가성 대장 — 초기 추출

재작성 뒤 이 표의 모든 행이 새 문서의 정확히 한 절에 있어야 한다("새 위치" 열은 재작성
뒤 채운다). 행이 없으면 누락, 표에 없는 보장이 새 문서에 있으면 추가 보장(둘 다 대조
실패).

### `24-runtime-monitoring` 유래 (R1–R34)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R1 | 이 문서는 특정 시점 완전한 status·status 변화 stream·structured log identifier 소유; 누적 수치는 25, message 한 건 진행은 26, relocation·shutdown 상태 전이는 30 소유 | 24 §1 | |
| R2 | 책임 표 4행(Application/Framework/Provider/Remote runtime) | 24 §1 | |
| R3 | descriptor revision·owner lease는 내부 판단에만 사용; public interface에 이 값들 + 작업 수락 내부 상태 + claim + capacity reservation + socket 상태 + exporter + 저장소 + raw event DTO + native handle 미노출 | 24 §1 | |
| R4 | Application은 등록 이름으로 기능별 status를 읽고 내부 service 값을 직접 조합하지 않음; 상태 범위별(Host/RouteMesh/ClientServer/Automatic fanout) 확인 값 표 | 24 §2 | |
| R5 | Status는 변경 불가능하며 호출 뒤에도 보관 가능; native handle·caller buffer·payload·application metadata 미참조 | 24 §2 | |
| R6 | 비규범적 C# 발췌 표기 규칙 — 다른 언어에 같은 signature 불요구, 정확한 type·signature는 언어별 exact interface 소유 | 24 §2 | |
| R7 | Host 상태는 특정 `MeshName`에 속하지 않음; relocation·shutdown 최종 결과는 host status에서 한 번만 제공 | 24 §2 | |
| R8 | Host runtime state 닫힌집합 7값(`preparing`/`serving`/`relocating`/`relocated`/`draining`/`stopped`/`error`), 표에 없는 값 추가 금지; `IsReady`는 `State=serving`일 때만 `true`; `AcceptingWork`는 별개 조건으로 재해석 금지 | 24 §2.1 | |
| R9 | `SafeToShutdown` — source가 자기 relocation operation의 두 조건(모든 unit이 Message Follow route 제거 가능 시점 도달 + 각 unit의 cutover 재전송 창 종료) 모두 끝난 뒤에만 게시; 두 판정에 다른 node 시각 미사용; target·다른 주체의 완료 ACK 아니라 source가 게시하는 값, deployment orchestrator가 §3 조회·관찰로 확인; 게시 전 `Shutdown` 호출 허용(이 경우 남은 Message Follow route가 transport와 함께 사라져 이전 route를 cache한 sender의 request가 `Unavailable`로 끝날 수 있음) | 24 §2.1 | |
| R10 | Host status의 capacity 항목은 같은 measurement epoch에서 Core HWM snapshot과 Application job queue snapshot을 coherent하게 읽음(queue 순회로 snapshot을 만들지 않음) | 24 §2.1 | |
| R11 | Core HWM snapshot 필드 목록(configured memory limit·manual budget·profile, effective budget, total applied HWM, core queue·current·provisional·peak accounted bytes, completion current·peak·pending, total messaging, monitor queue applied/accounted, total instance applied/accounted bytes, blocked ratio, active ordinary/completion/send/receive queue 수); 4개 reserved field(`application accounted bytes`/`outstanding application lease`/`retired queue`/`deferred origin credit`)는 ABI 호환용, 0.13.1 이후 항상 `0`, application byte HWM·lease 존재를 뜻하지 않음; Framework는 Core runtime snapshot을 그대로 투영하며 재계산·다른 의미로 미사용 | 24 §2.1 | |
| R12 | Application job queue snapshot 필드 목록(configured profile·manual max, pause·resume percent, effective processor count·max, pause·resume permit count, reserved supply permits, queued jobs, permits in use·peak, `running\|paused` pressure state, current pause duration, capacity waiter·wait count·duration); Reset은 configuration·pressure state·current pause duration 유지하며 epoch 증가, pressure transition count·cumulative pause duration·flow-state config failure count는 `0`으로; 동시 event는 이전 또는 새 epoch 중 정확히 하나, peak는 current보다 작을 수 없음 | 24 §2.1 | |
| R13 | Host status는 payload·Actor ID·Spot ID·session ID·RID·endpoint·message type이나 owner별 목록 미포함; owner별 top-N은 public contract로 미제공 | 24 §2.1 | |
| R14 | Topology state는 host state와 범위가 다름(process 전체 vs `MeshName`/`ChannelName` 단위); host가 `serving`이어도 특정 topology만 `degraded` 가능; host가 `relocating`/`relocated`/`draining`이면 연결이 남아 있어도 모든 topology의 `IsReady`는 `false`; 연결된 peer·target 수는 현재 연결 상태를 그대로 제공(traffic 안 받는다고 count를 `0`으로 안 바꿈) | 24 §2.2 | |
| R15 | Topology state(6값)·Topology reason(7값)·Peer state(5값)·ClientServer local role(3값) 닫힌집합 표 + Topology state별 의미(6행) | 24 §2.2 | |
| R16 | RouteMesh peer는 Node RID만 제공, Endpoint·descriptor revision·connection generation 미제공; `not_connected`(ready 집계 제외, liveness·health failure 집계 반영)와 `not_required`(ready 집계 제외, liveness probe·reconnect·health failure 집계 대상 아님) 구분; `not_required` peer도 status peer 목록에 남기며 이 상태 하나만으로 RouteMesh를 `degraded`로 안 바꿈 | 24 §2.2 | |
| R17 | RouteMesh placement 상태는 새 object 수락 여부 + 현재 active Actor·Spot 수 제공, type별 capacity reservation·activation barrier·내부 capacity counter 미제공; `IsAvailable`은 host가 `serving`이고 Object Server이며 placement weight가 양수이고 Actor·Spot capacity와 activation concurrency에 모두 여유가 있을 때만 `true`; activation concurrency 현재 값·limit은 별도 field로 미노출 | 24 §2.2 | |
| R18 | weight는 signed integer `0..10000`, `0`이면 새 placement 대상 미선택; `client_and_server`는 같은 `ChannelName`에 두 역할이 등록되었다는 뜻이며 별도 registration role 아님 | 24 §2.2 | |
| R19 | Automatic fanout publisher는 socket 연결 뒤 application record 또는 liveness beacon을 받으면 ready; disconnect 확인 또는 15초 동안 record 없으면 해당 publisher만 후보에서 제외; 연결 계획이나 `connect` 수락만으로 ready 안 됨 | 24 §2.2 | |
| R20 | 각 언어는 현재 status 조회와 비동기 변화 관찰 제공(이름·type은 언어별 exact interface); Status는 runtime instance 안에서 단조 증가하는 `Sequence`와 관찰 시각 포함, 같은 source는 큰 `Sequence`가 더 나중, 서로 다른 source 값은 비교하지 않음, process 재시작 시 `Sequence`는 0부터 시작 가능 | 24 §3 | |
| R21 | 변화 stream의 각 항목은 완전한 status(일부 field만 담은 event 아님), nullable field를 조합하는 범용 event DTO 미제공; `Sequence` gap 발견 시 현재 status를 다시 조회해 모든 field 복원 | 24 §3 | |
| R22 | source = `Sequence`를 소유한 것; Host 상태의 source는 이 runtime instance 하나(runtime instance ID, process 수명 동안 하나), Topology 상태의 source는 topology runtime 하나(RouteMesh는 `MeshName`, ClientServer·fanout은 `ChannelName`); peer와 객체 이동은 별도 source 아님(자기 `Sequence` 없이 topology status 목록에 실림, peer 하나가 바뀌면 그 topology status 전체가 새 `Sequence`로 발행); peer별·이동별 slot을 두려면 별도 stream과 언어별 계약을 먼저 정의해야 함 | 24 §3 "Source의 정의" | |
| R23 | Source 키는 처음 관측 대상이 될 때 만들고 terminal status가 전달되거나 폐기된 뒤 없앰; 키가 살아 있는 동안 `Sequence`는 그 키 안에서 단조 증가 | 24 §3 "Source의 정의" | |
| R24 | 합치기는 source별 최신 status 한 자리 유지(같은 source의 이전 중간 status는 최신 status가 대신함); 보관 중인 source에 대해 가장 최근 status의 `Sequence` 전달; relocation·shutdown의 terminal status는 중간 status로 덮어쓰지 않음; 한 관찰자의 지연·취소·실패가 다른 관찰자와 runtime 결과를 안 바꿈; 누계 field는 합친 뒤에도 최신 값 반영(backpressure·drop counter 증가분을 합치기로 안 잃음) | 24 §3 "합치기" | |
| R25 | 종료된 source의 terminal status 보관량에 상한이 있으며 넘기면 가장 오래된 terminal status부터 버림; 유실 수는 관찰자마다 달라 status 안에 안 넣음; stream이 전달하는 단위는 status와 유실 누계의 쌍; 유실 누계는 중간 status 합치기로 사라진 것과 terminal 폐기로 사라진 것을 각각 세며, 구독을 새로 시작하면 0에서 다시 시작, 표현 범위를 넘으면 최댓값으로 고정; runtime 전체 metric은 이 값을 대신할 수 없음(어느 관찰자가 무엇을 잃었는지 판정 못 함) | 24 §3 "합치기" | |
| R26 | Source의 최신 slot은 그 source lifecycle이 끝나고 terminal status가 전달·폐기된 뒤 제거(제거된 source는 "보관 중인 source"에서 빠짐); Framework는 관찰자 queue가 가득 찼다는 이유로 stream을 종료하지 않음(합치기와 보관 상한으로만 따라잡음); 관찰 취소는 해당 stream만 종료하며 이미 수락한 runtime 작업이나 다른 관찰자는 취소하지 않음 | 24 §3 "합치기" | |
| R27 | 운영 도구는 Actor ID 또는 Spot ID로 현재 위치를 정확히 조회하거나 page 단위로 열거 가능; 이 결과를 messaging target·placement selector로 미사용; 제공 field·page 크기·cache 계약은 Location runtime의 운영 조회(21 §6.4)가 소유; ID별 조회·page는 `Creating`/`Ready`/`Unavailable` entry를 같은 의미로 반환, record가 없으면 ID별 조회는 empty이고 page에는 항목 없음, Store 조회 실패는 `Unavailable` Framework error이며 page 일부를 성공 결과로 반환하지 않음 | 24 §4 | |
| R28 | Framework는 상태가 바뀐 이유를 표준 structured logger에 기록, Application이 provider·backend 구성; public interface는 sink·file path·exporter lifecycle·event DTO 미제공 | 24 §5 | |
| R29 | structured log identifier 12개 표(`zlink.runtime.*`, 모든 언어 동일 문자열) | 24 §5 | |
| R30 | Log는 timestamp·source 종류·등록 이름 기록, 필요한 변화에 Node RID·weight·reason·state 추가; payload·metadata·Actor ID·Spot ID·owner token·generation·raw frame·native handle 미기록 | 24 §5 | |
| R31 | Mailbox enqueue·dequeue·turn 시 structured log나 전용 metric 미기록; operation 실패는 drop·timeout·backpressure metric으로 집계, 개별 message 지연은 26에서 조사 | 24 §5 | |
| R32 | Publisher 상태는 `excluded_draining`/`excluded_stale`/`reconnecting`/`disconnected`로 기록; log는 기록 시점의 판단이며 현재 상태의 기준이 아님(현재 상태는 fanout status에서 읽음) | 24 §5 | |
| R33 | Relocation unit의 source admission seal부터 one-way cutover submit의 성공·실패 terminal까지 1초를 넘으면 `zlink.runtime.relocation.changed`에 `unit_kind`(`actor`/`instance_spot`/`user_spot`) + 필요 시 `execution_mode` + `interruption_target_exceeded=true` + 실제 duration 기록; 운영 경고이며 relocation outcome·recovery 판단을 안 바꿈; Actor ID·Spot ID는 structured log에 안 넣고 제한된 trace에서만 확인; target admission open은 source로 ACK 안 하며 target-local status·trace에서 관찰 | 24 §5 | |
| R34 | 등록하지 않은 `MeshName`·`ChannelName` status 요청, Manual subscriber 전용 fanout `ChannelName`에 automatic status 요청은 구성 오류; Location Store 없는 runtime은 store 상태 `not_configured`; Object role이 `Client`/`Server`인데 Location Store 없으면 host startup 실패; metric·trace를 꺼도 runtime status는 계속 사용 가능; Logger provider 실패는 message dispatch·reply·topology 조정·host lifecycle 결과를 안 바꿈 | 24 §6 | |

### `25-runtime-metrics` 유래 (R35–R50)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R35 | 이 문서는 처리량·대기·실패·현재 개수를 집계하는 metric의 이름·종류·단위·label 소유; 현재 완전한 상태는 24, message 한 건 진행은 26, host operation 개별 결과는 30 소유; 책임 표 3행(Application/Framework/Provider) | 25 §1 | |
| R36 | Label 값의 종류는 application object·message 수에 비례해 증가하지 않음; Exporter·registry·저장소·histogram bucket·backend는 public contract 아님 | 25 §1 | |
| R37 | `counter`/`updown`/`observable`/`histogram` 4종 정의; 계기 이름은 lowercase dotted ASCII `zlink.<surface>.<name>`; 이름·label key·허용 label value는 모든 언어에서 byte 단위로 같음; 시간 histogram 단위 `s`, byte 크기 단위 `By`, 나머지는 `{count unit}`; Provider failure는 application callback·reply·새 작업 수락·host lifecycle 결과를 안 바꿈 | 25 §2 | |
| R38 | Host Core HWM·job queue instance aggregate 계기 14개(`effective_budget`/`applied`/`accounted`/`completion_accounted`/`blocked_ratio`/`limit`/`jobs`/`capacity_waiters`/`capacity_waits`/`capacity_wait_duration`/`pressure_state`/`pressure_transitions`/`pause_duration`/`flow_state_config_failures`); Core runtime snapshot과 job queue accounting을 읽으며 queue·handler 순회로 수집하지 않음 | 25 §3.1 | |
| R39 | Reset은 pressure state와 current pause duration을 포함한 current gauge 유지, peak를 current로 재기준화, transition·cumulative duration·config failure를 포함한 epoch counter·누계는 `0`으로; Reset 시점에 이미 `paused`면 cumulative pause duration은 그 시점을 새 epoch 시작점 삼아 재누적 | 25 §3.1 | |
| R40 | Always-on metric은 job마다 timestamp·queue-wait histogram을 안 만듦; `MeshName`/`ChannelName`/Actor ID/Spot ID/session ID/RID/endpoint/packet name/owner는 label로 미사용 | 25 §3.1 | |
| R41 | Ready 계기는 기능별 serving 조건을 모두 만족한 peer·member만 포함; MeshNode descriptor가 configured peer 집계 기준; peer·channel 계기 8개(`peers.configured`/`peers.connected`/`peers.ready`/`channels.ready_members`/`channel.selection_failures`/`requests.inflight`/`request.duration`/`request.timeouts`) + label 표(`source` 3값/selection failure `reason` 3값/`surface` 5값) | 25 §3.2 | |
| R42 | One-way drop은 Framework가 원인을 확인할 수 있을 때만 횟수 기록(`zlink.mesh_node.messages.dropped`); `reason` 5값(`no_handler`/`decode_error`/`backpressure`/`stale_target`/`shutdown`); Logical Multicast와 classic fanout publish는 제외, target별 metric도 미생성 | 25 §3.3 | |
| R43 | `zlink.spot.count`/`zlink.actor.count`(updown, 이 MeshNode가 지금 실행 중인 수)와 `zlink.object.capacity.*`/`zlink.spot.type.capacity.*`(Location Store가 확정한 population)는 집계 경계가 달라 서로 대체하지 않으며 값이 다를 수 있음; capacity 계기는 active/reserved/limit(`0`이면 무제한) | 25 §4 | |
| R44 | `zlink.relocation.started`/`completed`/`duration`/`bytes`와 `zlink.stream.connections.active`/`opened`/`closed` 계기 표; label 표(`spot_kind`/`capacity_scope`/`stable_type`/`object_kind`/`policy`/relocation `outcome`/`transport`/`close_reason`) | 25 §4 | |
| R45 | Instance Spot 전용 계기 6개(`activations`/`activation.duration`/`pending.messages`/`pending.bytes`/`claim.conflicts`/`takeovers`); activation `outcome` 7값(`ready`/`rejected`/`conflict`/`timed_out`/`shutdown`/`store_failure`/`fenced`), claim `reason` 4값(`authority`/`spot_kind`/`spot_type`/`closing`), takeover `outcome` 3값(`claimed`/`lost`/`failed`) 닫힌집합 | 25 §4 | |
| R46 | `zlink.host.state`(observable, 현재 state 하나에 값 `1`); `zlink.host.relocation.duration`/`blocked`; `zlink.relocation.interruption`(`unit_kind`별, 1초 초과를 relocation failure로 안 바꿈)/`target_resume`/`route_convergence`/`cutover_timeout`; `zlink.host.shutdown.duration`/`forced`; `state` 7값/relocation `outcome` 2값(`relocated`/`blocked`)/shutdown `outcome` 2값(`stopped`/`force_stopped`) 닫힌집합, reason은 30의 식별자를 사용 | 25 §5 | |
| R47 | Relocation 구간 지표는 3구간으로 나눔 — source 정지 구간(=`zlink.relocation.interruption`, 별도 계기 추가 없음), target 재개 구간(`target_resume`)과 route 수렴 구간(`route_convergence`)은 각 node가 자기 clock으로 측정해 자기 지표로 게시(서로 다른 node 시각을 직접 빼는 지표는 없음, node를 가로지르는 전체 중단 구간은 26의 같은 flow 상관으로 관찰); §7의 label cardinality 규칙이 이 지표에도 적용; `zlink.relocation.cutover_timeout`이 `0`이 아니면 relay 순서를 보장하지 않는 fallback 경로가 실제로 쓰이고 있다는 뜻(`RelocationCutoverWaitTimeout` 조정 판단 근거) | 25 §5 | |
| R48 | `zlink.location.store.errors`/`owner_lease.renew.failures`/`renew.lateness`/`zlink.observability.events.overflow` 계기 표; `scope_kind`(`mesh`/`channel`), `operation` 7값(`read`/`compare_exchange`/`relocation_put`/`relocation_get`/`relocation_delete`/`lease_renew`/`release`) 닫힌집합; Logical Multicast·classic fanout publish는 집계 안 함 | 25 §6 | |
| R49 | Label에는 startup 등록값이나 enum 허용 값만 사용; 허용/금지 label 표(허용 다수 vs `topic`/Actor ID/Spot ID/RID/endpoint/session ID/relocation ID/user ID/`correlation ID`/`flow ID`/application metadata value/application state format·version 금지); `MeshName`/`ChannelName`/`scope_name`은 host 등록값으로 닫혀 있을 때만 사용; payload에서 label을 안 만듦; 개별 흐름은 26에서 확인 | 25 §7 | |
| R50 | 각 언어는 표준 meter·registry 사용, public API는 exporter·reader·storage·histogram bucket을 구성하지 않음; metric을 끈 경로는 payload를 복사하거나 per-message label dictionary를 안 만듦; counter·updown 갱신은 dispatch ordering을 안 바꿈; observable은 이미 유지하는 제한된 크기의 집계값만 읽음(Actor·Spot·mailbox·Store record 전체 순회 없음); mailbox enqueue·dequeue·turn마다 기록 안 함; provider가 histogram bucket·aggregation 결정; provider callback failure는 마지막 정상 수집 결과를 소급해서 안 바꿈 | 25 §8 | |

### `26-message-flow-tracing` 유래 (R51–R71)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R51 | 이 문서는 message 한 건이 어느 처리 단계에 도달했고 어디서 실패했는지를 trace·structured log로 확인하는 계약 소유(기존 전달·완료 보장을 안 바꿈); Application이 기록 수준·정상 흐름 선택 비율·byte 크기 기록 여부 설정, Framework가 각 경계에서 기록을 만들어 표준 경로로 전달; provider는 처리 지연·결과 변경 금지, exporter·저장소·observer·event DTO 미노출; 24는 runtime health·lifecycle, 25는 집계 수치, 27은 `correlation_id`·`flow_id` 생성·전파·수명 소유(이 문서는 두 식별자를 기록에 넣는 조건만) | 26 §1 | |
| R52 | 일반 처리 단계는 `zlink.message_flow`, payload 해석·handler·reply 전달 경로·protocol 분배 실패는 `zlink.dispatch_error`; 두 `event_id` 문자열은 모든 언어에서 같음 | 26 §2 | |
| R53 | message flow `phase` 닫힌집합 9값(`received`/`admitted`/`dispatched`/`completed`/`replied`/`sent`/`reply_received`/`backpressured`/`dropped`) + sequence diagram(Source/Transport/Queue/Handler 4주체); `sent`는 remote handler 수신을 뜻하지 않고, `admitted`는 handler 완료를 뜻하지 않으며, request는 `reply_received`에 도달해야 caller가 terminal reply를 받은 것 | 26 §2.1 | |
| R54 | Logical Multicast와 Classic fanout은 subscriber별 결과를 확인하지 않으므로 message-flow trace를 만들지 않음 | 26 §2.1 | |
| R55 | 기록하는 public 동작 6그룹(Node direct+Channel/Spot direct/Instance Spot/Actor+relocation/STREAM session/Request timeout·cancellation·종료·분배 오류); wrapper와 transport는 같은 terminal trace를 중복 생성하지 않음(request에는 surface별 terminal 기록이 정확히 하나); Actor payload는 Spot의 handler 분배 단계로 기록하지 않음 | 26 §2.2 | |
| R56 | `surface` 8값/`message_kind` 5값/`outcome` 6값/`channel_route_kind` 2값/`activation_state` 3값 닫힌집합(대소문자 포함 모든 언어에서 같음); `shutdown`의 의미 | 26 §3.1 | |
| R57 | `zlink.message_flow`의 `reason` 7값(`backpressure`/`stale_target`/`target_closed`/`shutdown`/`location_unavailable`/`activation_rejected`/`activation_timeout`), 마지막 세 값의 의미(Instance Spot 위치 못 찾음/생성 거부/생성 시간 초과); Instance Spot close·lease fencing은 `target_closed`로 기록 | 26 §3.1 | |
| R58 | `zlink.dispatch_error`는 항상 `outcome=failed`; `reason` 9값(`no_handler`/`decode_error`/`handler_exception`/`invalid_frame`/`reply_path_missing`/`unexpected_reply`/`backpressure`/`stale_target`/`shutdown`); `action` 3값(`reply_error`/`fail_caller`/`drop`) | 26 §3.1 | |
| R59 | Attribute 포함 조건 표 17항목(`event_id`/`timestamp`/`phase`/`surface`·`message_kind`·`outcome`/`reason`/`action`/`channel_name`/`channel_route_kind`/`mesh_name`/`server_rid`/`source_rid`·`target_rid`/`packet_name`/`topic`·`spot_id`·`actor_id`/`instance_spot_type`·`activation_state`/`correlation_id`/`flow_id`·`flow_origin`/`message_size_bytes`/`duration_seconds`) | 26 §3.2 | |
| R60 | `channel_route_kind`·`mesh_name`·`server_rid`는 handler를 찾거나 target을 선택하는 입력이 아님; trace에 payload·application metadata 값·native handle·raw frame·exception object 미기록; error 설명 문자열은 구현이 정한 최대 길이 안에서 제한하며 secret·stack trace 미포함 | 26 §3.2 | |
| R61 | Structured log를 대신 제공하는 구현은 `zlink flow:` prefix와 고정 key 22개를 그대로 사용 | 26 §3.2 | |
| R62 | Logical Multicast·Classic fanout의 정상 publish·subscriber delivery는 `zlink.message_flow`를 만들지 않음; Classic fanout subscriber의 local dispatch에서 handler가 없으면 `surface=classic_fanout`/`message_kind=send`/`outcome=failed`/`reason=no_handler`/`action=drop`인 `zlink.dispatch_error`를 subscriber process의 logger provider에 기록, `channel_route_kind`는 안 넣고 publisher별 delivery 결과로 되돌리지 않음 | 26 §3.2 | |
| R63 | diagnostics level 닫힌집합 4값(`Off`/`Errors`/`Normal`/`Detailed`) 표, 기본값 `Errors`; message size 설정은 payload 내용이 아니라 byte 크기만 추가; diagnostics level은 metric 기록을 안 끔 | 26 §4 | |
| R64 | Sampling rate는 `0.0..1.0`, 범위 밖은 startup·public 인자 오류; `flow_id`의 hash로 정상 흐름을 선택(같은 흐름의 hop은 모두 기록되거나 모두 제외); `zlink.dispatch_error`·`backpressured`·`dropped`는 sampling 안 함; `flow_id`가 없으면 source MeshNode generation과 local sequence로 기록 여부 결정 | 26 §4 | |
| R65 | 비규범적 C# `IZLinkDiagnosticsOptions` 발췌(`SetLevel`/`SetSampleRate`/`IncludeMessageSizes`); public configuration은 level·sampling rate·message size 포함 여부만 제공, exporter·logger provider·저장 backend는 표준 telemetry configuration 소유 | 26 §4 | |
| R66 | Application은 process 재시작 없이 diagnostics level 변경 가능; surface마다 별도 toggle 없음(process 안 모든 Node·Channel·Spot·Actor·STREAM에 함께 적용); 변경은 message 처리를 안 기다리는 원자적 상태 변경; 각 처리 지점은 trace 데이터를 만들기 전 현재 level을 한 번 확인, 확인한 지점부터 새 설정 적용; 변경 전 이미 telemetry queue에 들어간 기록은 전달·폐기 가능; level을 다시 켜도 이전 처리 단계 기록을 나중에 안 만듦 | 26 §4.1 | |
| R67 | `Off`에서는 현재 level 확인 읽기·분기 외 trace 전용 작업을 안 함 — event 객체·attribute collection 미생성, 문자열 조합·timestamp·경과 시간 수집 안 함, payload·metadata 복사·크기 계산 안 함, sampling hash·trace 전용 `flow_id` 미계산, structured log message 미생성, telemetry queue item·내부 전달 message 미생성, logger·observer·exporter·provider 미호출; log provider에서 출력만 막는 구현은 `Off` 계약을 만족하지 않음(message hot path의 첫 trace 분기에서 종료해야 함) | 26 §4.1 | |
| R68 | `sent`/`admitted`/handler 완료/reply 수신은 서로 다른 완료 경계; trace 기록 자체의 성공 여부는 message operation의 완료 조건에 미포함; timeout·cancellation·shutdown은 원래 message operation의 계약대로 결과를 정하며 tracing은 retry·route 재선택을 추가하지 않음(routing·handler 분배·lifecycle 결정을 안 바꿈) | 26 §5 | |
| R69 | Worker는 느리거나 실패한 telemetry provider를 안 기다림; 크기가 제한된 telemetry queue가 가득 차면 정상 trace를 버리고 `zlink.observability.events.overflow`를 증가시킬 수 있음; Provider failure는 message operation failure가 아님 | 26 §5 | |
| R70 | Provider failure를 log로 남기는 구현은 같은 오류의 기록 횟수를 제한(같은 provider를 호출해 이 log의 trace를 다시 안 만듦); provider가 없으면 trace만을 위한 allocation을 피함; provider failure 자체를 기록할 때는 실패한 provider가 아닌 application의 fallback logger 또는 process stderr 사용, 이 fallback 기록의 실패도 message operation 결과·dispatch·lifecycle을 안 바꿈 | 26 §5 | |
| R71 | Trace attribute에는 진단에 필요한 식별자만 넣으며 message 처리가 끝난 뒤 caller buffer·runtime object를 미참조; `correlation_id`·`flow_id`·`flow_origin`의 소유권·수명은 27이 소유, 세 값을 metric label로 미사용 | 26 §5 | |

### `27-flow-correlation` 유래 (R72–R90)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R72 | 이 문서는 request와 terminal reply를 정확히 연결하고 같은 원인에서 이어진 여러 message를 하나의 업무 흐름으로 식별하는 계약(생성·형식·전파·소유권·수명)을 소유; Application은 이 식별자를 생성하거나 reply 연결에 미사용; message metadata의 소유권·크기는 04, trace에 field를 넣는 조건·sampling은 26이 소유; 세 field는 Framework가 관리하는 context이며 application metadata key 아님 | 27 §1 | |
| R73 | `correlation_id`/`flow_id`/`flow_origin` 역할 표(연결 범위/만드는 주체/유효 기간); Framework는 `correlation_id`만 사용해 reply를 현재 대기 중인 request와 연결; `flow_id`는 관측용이며 message 중복 제거·idempotency·현재 owner 검증에 미사용 | 27 §2 | |
| R74 | Downstream request(sequence diagram, Origin/Handler/Downstream 3주체)마다 새 `correlation_id`를 만들되 같은 원인에서 이어졌으면 `flow_id`는 유지; reply가 없는 one-way message에는 `correlation_id`를 안 만듦 | 27 §2 | |
| R75 | `correlation_id` 형식(Framework가 만드는 `1..64 byte` opaque ASCII, 만든 runtime의 같은 lifecycle에서 동시 대기 중인 request 사이에 중복 불가); `flow_id` 형식(소문자·hyphen UUIDv7, 정확히 `36 ASCII byte`); `flow_origin` 4값(`inbound`/`timer`/`application`/`lifecycle`), 흐름을 처음 만들 때 정한 값을 이후 hop에서도 유지 | 27 §3 | |
| R76 | Application은 세 값을 해석·조립하지 않음; `flow_id`와 `flow_origin`은 함께 존재하거나 함께 없어야 함; 형식이 잘못된 `flow_id`, byte 길이가 0인 `correlation_id`, 두 field 중 하나만 있는 flow 정보는 protocol error(Framework message envelope는 해당 operation을 `ProtocolError`로 완료, STREAM frame은 connection을 `ProtocolError`로 종료) | 27 §3 | |
| R77 | Inbound message에 형식이 올바른 `flow_id`·`flow_origin`이 있으면 그대로 사용(단, 현재 runtime의 message-flow tracing이 켜져 있을 때만 두 field를 읽고 flow context에 넣음); 두 field가 없으면 다음을 새 흐름의 시작으로 봄 — STREAM ingress와 Node·Channel·Spot·Instance Spot·Actor의 inbound 처리, Timer·lifecycle callback, Framework callback 밖 application code가 시작한 첫 outbound operation | 27 §4 | |
| R78 | Diagnostics level이 `Off`이면 관측 전용 flow 처리를 모두 생략 — 새 `flow_id`를 안 만들고 inbound message의 flow field를 flow context로 만들거나 다음 message에 복사하지 않으며, outbound envelope에도 두 field를 추가하지 않음(client connector가 시작한 outbound request도 동일 규칙) | 27 §4 | |
| R79 | `correlation_id`는 request와 terminal reply를 연결하는 protocol 정보로, diagnostics level이 `Off`여도 request마다 만들고 reply까지 보존(tracing을 끌 때 제거할 수 없음) | 27 §4 | |
| R80 | Framework는 callback 실행을 시작할 때 현재 flow context를 설정하고 terminal completion에서 실행 전 context로 복원, tracing이 `Off`이면 이 context를 만들거나 async-local storage에 안 넣음; 실행 중 diagnostics level 변경 규칙은 26 §4.1을 따름 — 각 처리 지점이 `Off`를 확인한 뒤에는 flow ID 생성·validation·context capture·envelope field 추가·전파용 내부 message 생성을 안 함, 이미 만들어진 outbound frame에는 변경을 소급 적용하지 않음 | 27 §4 | |
| R81 | Message-flow tracing이 켜져 있으면 한 작업에서 원인과 결과가 이어지는 동안 `flow_id`·`flow_origin`을 함께 전달; 처리 경계별 보존 범위 표 7행(Node direct+Channel/Spot direct/Instance Spot direct/Actor direct+STREAM/Actor relocation/현재 session 연결 push/Logical Multicast+Classic fanout) | 27 §5 | |
| R82 | Logical Multicast·Classic fanout은 branch마다 target identity·local sequence가 달라도 `flow_id`는 같음 | 27 §5 | |
| R83 | 중간 runtime이 원래 request를 전달할 때는 terminal reply까지 원래 `correlation_id`를 보존; downstream request에는 새 `correlation_id` 사용; tracing이 켜져 있고 현재 flow context가 있으면 두 flow field도 전달 | 27 §5 | |
| R84 | Instance Spot을 처음 선택한 target이 생성 권한을 얻지 못하면 현재 요청을 받을 수 있는 Ready owner로 message를 한 번 전달 가능(원래 `correlation_id` 유지, tracing이 켜져 있으면 `flow_id`·`flow_origin`도 유지); target queue가 message를 수락한 뒤에는 Framework가 자동으로 다시 전송하지 않음 | 27 §5 | |
| R85 | Framework가 기다리는 비동기 continuation에는 현재 flow context를 보존; 분리 실행한 task·별도 executor·외부 callback에는 context를 암묵적으로 전달하지 않음(명시적으로 전달한 context가 없으면 새 application flow로 처리하되 tracing이 켜져 있을 때만 새 flow를 만들고 context를 보존); async-local context를 안전하게 지원 못 하는 언어는 context를 명시적으로 capture하는 public interface 제공, process-global 변수·thread ID·변경 가능한 connector field로 현재 flow를 안 추정 | 27 §6 | |
| R86 | Response와 error는 request의 `correlation_id`를 보존; reply를 만드는 시점에 tracing이 켜져 있고 request flow context가 있으면 `flow_id`·`flow_origin`도 보존; request가 reply·error·timeout·cancellation·shutdown으로 terminal 완료되면 해당 `correlation_id`를 더 이상 reply 연결에 미사용 | 27 §7 | |
| R87 | Timeout·cancellation 뒤 도착한 reply는 다른 pending request와 미연결; 연결이 교체된 뒤 이전 STREAM session의 reply·push도 새 session의 flow와 미연결; binding token이 더 이상 유효하지 않은 경우에도 reply·push를 새 session의 flow에 미연결; dispatch failure를 기록할 수 있으면 실패한 message에서 읽은 correlation·flow 정보를 유지; invalid frame에서 식별자를 못 읽으면 새 식별자를 만들어 원래 request의 기록처럼 표시하지 않음 | 27 §7 | |
| R88 | Downstream terminal completion은 이를 시작한 원래 activation에 정확히 한 번만 전달; 해당 operation이 확인한 generation이 바뀌거나 owner가 종료되면 stale 결과로 끝냄; timeout·cancellation·늦은 reply는 handler 분배를 다시 실행하거나 route를 다시 선택하게 하지 않음 | 27 §7 | |
| R89 | `flow_id`가 같다는 사실은 retry를 허용하지 않음; retry 여부와 새 `correlation_id` 발급은 해당 messaging surface의 계약을 따름 | 27 §7 | |
| R90 | Tracing은 `correlation_id`·`flow_id`·`flow_origin`을 기록(정확한 포함 조건·structured log key는 26 §3.2 소유); metric label에는 세 값을 모두 미사용; 세 field에는 user ID·Actor ID·Spot ID·endpoint·payload·application metadata를 미encode; 외부 trace adapter도 Framework가 정한 형식·소유권을 안 바꿈 | 27 §8 | |

### 네 문서의 검증 요구 절 — 본문에 없는 고유 규칙 (R91–R94)

각 문서 마지막 절(§7/§9/§6/§9)의 항목 대부분은 위 R행이 이미 다루는 규칙의 관찰 방법
재진술이라 별도 행을 만들지 않는다. 다음 네 항목은 **검증 요구 절에만 있고 본문
어디에도 없는 독립 규칙**이므로 각자 행을 둔다 — 재작성 시 이 문장이 검증 요구 절
밖으로 사라지지 않게 확인한다.

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| R91 | Publish target 수와 target별 수락·실패 결과를 status나 runtime structured log에 포함해서는 안 됨(24 본문 §2.2·§5 어디에도 없는 독립 금지 규칙) | 24 §7 | |
| R92 | Instance one-way activation 실패는 `surface=instance_spot` drop이며 reply나 replay를 만들지 않음; Host 계기와 label은 `30-host-relocation-flow`의 result와 일치함(교차 문서 정합 요구, §6 S13과 연결) | 25 §9 | |
| R93 | Instance Spot의 one-way 생성 실패는 `surface=instance_spot`, `phase=dropped`로 정확히 한 번 기록하고 숨은 request나 replay를 만들지 않음(26 §3.1의 reason 설명과 26 §2.2의 기록 대상 목록에는 이 "정확히 한 번" 보장이 명시되어 있지 않음) | 26 §6 | |
| R94 | 실행 중 tracing을 끈 뒤 새 처리 지점이 기존 flow 정보를 context·outbound envelope에 추가하지 않고, 다시 켠 뒤에는 이전 단계를 소급하여 기록하지 않는지를 검증 대상으로 명시(27 본문 §4·§4의 소급 금지 문장과 짝을 이루는 독립 확인 항목) | 27 §9 | |

### 최상위 README "디버깅 원칙" 유래 (R95–R99)

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| R95 | 간헐 실패를 쫓을 때는 이미 있는 message tracking·파일 log를 먼저 켜고 읽는다(임시 log를 새로 넣고 재현을 반복하는 방식 금지); 먼저 켜는 대상 표 4행(Message flow mode/C++·.NET spot discovery trace/Java·Kotlin stream trace/Sample 서버 log 보존); 첫 재현부터 서버 log 보존 | README.ko.md §"디버깅 원칙" 1 | |
| R96 | 어떻게 읽는가 — `flow`로 정상 건과 실패 건을 나란히 놓고 어느 전이에서 끊겼는지 찾음(`flow`는 message 하나를 process 경계 너머까지 잇는 유일한 값, trace 종류를 noise로 보고 grep에서 걸러내지 않음) | README.ko.md §"디버깅 원칙" 2 | |
| R97 | 실패는 반드시 flow에 남긴다 — error kind만 돌려주고 원인을 버리는 종결을 만들지 않음; 실패·거부·abort 같은 종결은 `message_flow_outcome`의 `error`로, 원인 exception을 `errorType`·`errorMessage`에 실어 그 실패를 만든 message와 같은 `flow` 아래 기록. **주의**: `message_flow_outcome`·`errorType`·`errorMessage`는 26의 attribute 표(§3.2)에도 `zlink flow:` key 목록(§3.2)에도 없는 이름이다 — 재작성 시 이 세 이름을 새로 만들지 말고(가이드 §2.5) 26이 실제로 쓰는 이름(`outcome=failed`, `reason`, 구현이 정한 길이 제한 안의 error 설명 문자열)에 맞춰 다시 쓴다. 근본 원인은 spec-gap 후보 G3으로 별도 등록 | README.ko.md §"디버깅 원칙" 3 | |
| R98 | 결정: Message flow tracing이 꺼져 있으면 log message를 만드는 비용 자체가 없어야 함; message마다 찍는 hot path는 `if(enabled(outcome))`로 감싸 event·lambda 모두 미생성; 실패·abort 등 드문 전이는 lazy 형태(`trace(outcome, build)`/`traceLazy`)로 gate 통과 후에만 event 생성; lazy 형태도 hot path에서는 `if`로 한 번 더 감싸 lambda 생성까지 막음; 문자열 연결을 gate 앞에서 실행하는 호출부를 만들지 않음(R66·R67과 병합) | README.ko.md §"Trace 비용 규칙" | |
| R99 | 언어별 재량 — gate를 표현하는 방법(C++ template lambda/.NET 보간 문자열 handler+`Func<>`/Java `Supplier<>`/Node thunk), 관찰되는 결과(꺼졌을 때 비용 0)가 같으면 됨; 확인할 결과 — 새 trace를 넣은 뒤 tracing을 끈 상태에서 그 경로가 문자열·event·lambda 중 어느 것도 안 만드는지 호출부 코드로 확인(R66·R67과 병합) | README.ko.md §"Trace 비용 규칙" | |

## 6. 링크·코드·site 영향

| 대상 | 처리 |
|---|---|
| 스펙 내부 링크(다른 spec/server 문서가 24/25/26/27을 참조) | 새 경로·새 절 anchor로 치환. 절 제목이 대부분 그대로 유지되므로(§3 표에서 확인) anchor 치환표는 §1·S1·S4·S7에서 이름이 바뀐 절만 다룬다 |
| 외부 133 `.md` 파일, 서로 다른 anchor 16종(4+2+5+5) | 같은 치환표로 sed. 언어별 guide는 `generate_language_guides.py`가 공통 guide에서 생성하므로 **공통 guide만 고치고 재생성** |
| `contract-inventory`의 `.json` 3개(§1) | 경로 문자열만 치환하는 sed 대상에 포함(스펙 anchor는 없음) |
| 최상위 README | "디버깅 원칙"·"Trace 비용 규칙" 절 삭제, `06-observability/README.ko.md`로 가는 한 줄 링크로 대체(캠페인 말미 site 작업에서 README를 1장으로 축소할 때 함께 처리해도 됨 — 이번 주제 완료 시점에는 옛 문서를 손대지 않는 원칙(§4.6)을 따르므로 README도 아직 안 고침) |
| cpp layout contract test | 이 문서 4개를 needle로 쓰지 않으므로 갱신 대상 없음(§1 확인 완료) |
| mkdocs nav | "Observability" 그룹 → `observability/README`, `runtime-monitoring`, `runtime-metrics`, `message-flow-tracing`, `flow-correlation` |
| redirect | 캠페인 말미 site 작업에서 `24-runtime-monitoring`→`observability/runtime-monitoring`, `25-…`→`observability/runtime-metrics`, `26-…`→`observability/message-flow-tracing`, `27-…`→`observability/flow-correlation` |
| 교차 주제 확인(§4 S10–S13) | `29-transport-liveness`(channel-transport 주제)의 15초 peer deadline, stream-connector §6.3(session이 이미 인용)의 `close_reason` 닫힌집합, `28`·`30`(location-relocation 주제)의 `SafeToShutdown` 조건과 relocation reason·outcome 식별자 — 해당 주제 재작성 시 이 문서의 링크·값이 맞는지 재확인 |
| 검증 | `check_doc_links.py`, `mkdocs build --strict`, `git diff --check` |

## 7. 구현 대조 — 에이전트 입력

재작성이 끝나면 언어별 에이전트 4개(dotnet·jvm·cpp·node)에 다음을 준다.

- 입력: 새 `runtime-monitoring`, `runtime-metrics`, `message-flow-tracing`, `flow-correlation` 네
  문서와 §5 대장(새 위치 열 채운 것)
- 과제: 대장의 행마다 해당 언어 구현에서 **일치 / 불일치 / 스펙 미정 / 판단 불가** 중 하나와
  근거(파일:줄). 특히 계기 이름·label·structured log identifier·attribute key는 문자열이
  byte 단위로 같은지 확인(R37·R56·R59·R61 등)
- 금지: 스펙 수정, 코드 수정. 판정은 하지 않고 사실만 보고
- 출력 양식: `| R# | 판정 | 근거 | 비고 |`

판정(Claude): 옛 문서·4개 구현이 일치하는데 새 문서만 다르면 **문서 오류** → 수정.
옛 문서 때부터 달랐거나 언어끼리 다르면 **spec gap** →
[spec-gap 대장](../../spec-gap.ko.md) 등록, 문서는 계약 의도대로 유지.

## 8. 작업 순서

1. `06-observability/README.ko.md` 초안(§3.0 책임 지도 + §2 질문표 + §3.5 진단 순서) —
   문서 소속·중복 판정의 최종 확인
2. `01-runtime-monitoring` 재작성(ko) → §5 대장 R1~R34 새 위치 채움
3. `02-runtime-metrics` 재작성(ko) → R35~R50
4. `03-message-flow-tracing` 재작성(ko, README "Trace 비용 규칙" 병합) → R51~R71, R94~R95
5. `04-flow-correlation` 재작성(ko) → R72~R90
6. README 이관 나머지(R91~R93, 진단 순서) 반영
7. 등가성 대조 — 대장 빈 행 0, 추가 보장 0
8. en 짝 작성
9. 링크 치환·guide 재생성·nav → 검증 3종 그린(cpp contract test는 이 주제와 무관)
10. 구현 대조(§7) → 판정·기록
11. 한 커밋(문서 이동+내용, 옛 README 두 절 삭제 포함) + spec-gap 대장 갱신

## spec-gap 후보

| # | 내용 | 출처 |
|---|---|---|
| G1 | `25-runtime-metrics` §8("Metric을 끈 경로는…")과 `24-runtime-monitoring` §6("Metric이나 trace를 끄더라도…")이 "metric을 끈다"는 전제를 두 번 쓰지만, metric 전용 on/off 설정(이름·기본값·범위)이 이 네 문서 어디에도 정의되어 있지 않다. `26-message-flow-tracing` §4는 diagnostics level이 message-flow trace 전용이며 "diagnostics level은 metric 기록을 끄지 않는다"고 명시하므로, metric을 끄는 것이 diagnostics level과 별개의 설정이라는 것만 알 수 있고 그 설정 자체는 스펙에 없다. | `25-runtime-metrics.ko.md:278`, `24-runtime-monitoring.ko.md:369` |
| G2 | `24-runtime-monitoring` §2.1의 Core HWM snapshot 필드 목록(문단)에 있는 "monitor queue applied/accounted", "total instance applied/accounted bytes", "active ordinary/completion/send/receive queue 수"에 대응하는 지속 계기가 `25-runtime-metrics` §3.1의 계기 표에 없다(§3.1은 `effective_budget`/`applied`/`accounted`/`completion_accounted`/`blocked_ratio` 5개만 계기로 노출). 1회 조회(status)에서만 보이고 시계열 metric으로는 의도적으로 제외한 것인지, 정의가 빠진 것인지 스펙에 근거가 없다. | `24-runtime-monitoring.ko.md:126-133`, `25-runtime-metrics.ko.md:54-69` |
| G3 | 최상위 README "디버깅 원칙" §3(R97)은 `message_flow_outcome`의 `error`, `errorType`, `errorMessage`라는 field 이름으로 실패를 기록하라고 지시하지만, 이 이름들은 `26-message-flow-tracing`의 attribute 표(§3.2)에도 structured log 대체 key 목록(§3.2, `zlink flow:` prefix 22개)에도 없다. 26이 실제로 정의하는 것은 `outcome=failed`(닫힌집합 6값 중 하나)와 `reason`, 그리고 "구현이 정한 최대 길이 안의 error 설명 문자열"이다. README가 26이 소유하지 않는 이름을 계약처럼 적어 둔 것인지, 26 쪽에 이 세 field가 빠진 것인지 스펙 사이에 정합이 없다. | `README.ko.md:250-252`(디버깅 원칙 §3), `26-message-flow-tracing.ko.md:143-177`(§3.2) |
