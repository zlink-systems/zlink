# spec/server 재구성 — channel-transport 매핑표

> 캠페인: `framework/doc/framework/common/spec/server`를 주제별로 재구성하고
> [스펙 문서 작성 가이드](../../../../principal/documentation/spec-writing-guide.ko.md)대로 다시 쓴다.
> 이 문서는 `02-channel-transport` 주제의 작업 계획이다. 양식은
> [04-session 파일럿 매핑표](../04-session/mapping.ko.md)를 따른다.

관련: [spec-gap 대장](../../spec-gap.ko.md) · [주제 구분 초안](../../topic-map.ko.md) ·
[전체 목차 초안](../../target-readme.ko.md)

## 1. 대상과 규모

| 현재 문서 | 줄 수 | 성격 | 외부 anchor 링크 수 |
|---|---|---|---|
| `07-channel-topology` | 726 | 계약 — RouteMesh 물리 연결, ChannelName membership, peer 연결·discovery | 8 |
| `08-channel-messaging` | 615 | 계약 — Node direct와 ChannelName select-one 공통 계약, handler 조회, metadata | 13 |
| `09-client-server-channel` | 455 | 계약 — ClientServer Client/Server role, weight 선택, drain, 재시작 | 6 |
| `10-network-listener-identity` | 350 | 계약 — bind/advertise 주소, port 확정, RID·Spot ID 발급 정책(§7~7.3) | 6 |
| `29-transport-liveness` | 320 | 계약 — probe/ack·beacon 고정 시간, ready·장애 판정, reconnect | 17 |
| `49-internal-liveness-and-state` | 255 | 구현 스펙 — liveness 단일 기준(§1), 시작 순서·상태 공개(§2~3), 구독 backpressure(§4), 계측 비용(§5) | 7 |
| `51-internal-service-wire-protocol` | 750 | 구현 스펙 — wire schema, framing, command 53개, admission, liveness wire, relocation manifest·CAS | 8 |
| **합계** | **3,471** | | **65** |

(외부 anchor 링크 수는 `grep -rho "](파일명\.ko\.md[^)]*)" --include="*.md" .`로 저장소 전체에서 셌다.
7개 문서를 참조하는 파일은 spec/server 디렉터리 내부 23개뿐이었다 — session 주제와 달리 언어별
guide·e2e·sample 문서가 이 7개 문서를 직접 참조하지 않는다.)

### 코드에서 경로로 여는 곳

`topic-map.ko.md` §7과 README §7이 명시한 그대로다. `08-channel-messaging.ko.md` §3.2를 주석에
경로로 담은 곳이 두 언어에 있다 — 재작성 후 경로와 절 제목을 함께 갱신해야 한다.

| 파일:줄 | 인용 내용 |
|---|---|
| `framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts:629` | `` `framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md` §3.2 requires: the call waits `` |
| `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRuntime.java:1100` | `` per {@code framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md} §3.2: the call `` (주석 원문에 broken 한글 "짠" 오타 있음 — 문서 이동과 무관하지만 재작성 에이전트가 손대지 않는다) |

두 주석 모두 08 §3.2 "ChannelName select-one"의 **local Server 대기 규칙**(ready 후보가 없으면
`min(request timeout, 5초)` 동안 기다린다)을 가리킨다. cpp layout contract test는 이 7개 문서를
경로로 열지 않는다(README §7에 적힌 대상은 `14-actor-model`, `06-framework-api`,
`20-session-actor-dispatch`뿐).

## 2. 독자 질문 — 주제 README가 답할 것

가이드 §1 질문표를 channel-transport 주제에 맞게 채운 것. `target-readme.ko.md`가 이미 6개 새 문서
줄과 한 줄 소개를 잡아 두었으므로 그 소개문을 질문표의 출발점으로 쓴다.

| 질문 | 답이 있어야 할 자리 |
|---|---|
| RouteMesh의 물리 연결(MeshNode·ROUTER)과 ChannelName의 논리 membership은 어떻게 다른가 | README 개요 + `channel-topology` §1~§2 |
| 같은 MeshName인데 자동으로 중계되지 않는 경우는 언제인가 | `channel-topology` MeshName·MeshNode 절 |
| Node direct와 ChannelName 호출은 대상을 어떻게 고르는가 — 무엇이 같고 무엇이 다른가 | `channel-messaging` target 선택 절 |
| ClientServer는 RouteMesh와 무엇이 다른가 — 누가 연결을 시작하고 누가 응답만 하는가 | `client-server-channel` §1, §4 |
| weight와 drain은 선택에 각각 어떻게 반영되는가 | `channel-messaging` 선택 순서, `client-server-channel` §5, `channel-topology` §4.3 |
| listener의 bind 주소와 advertised 주소는 왜 다르고 언제 각각 필요한가 | `listener-identity` §3~§4 |
| MeshNode RID와 Entry Spot ID는 어떻게 발급되고 왜 UUID를 쓰는가 | `listener-identity` §7 (범위 재검토 대상 — §4 S8 참조) |
| 연결이 살아 있는지는 어떻게 계속 확인하고, 판정 기준은 무엇인가 | `transport-liveness` §2~§3 |
| Classic fanout은 왜 다른 방식(beacon)으로 확인하는가 | `transport-liveness` §4 |
| 연결이 끊기면 무엇을 다시 하고 무엇을 재사용하지 않는가 | `transport-liveness` §6 |
| 실패하면 무엇이 남는가(재전송 안 함, 한 번만 완료) | 각 문서 실패 절 + `transport-liveness` §6 |
| node 사이에 실제로 오가는 byte와 command는 무엇인가 | `wire-protocol` §1~§5 |
| relocation·actor join의 wire 세부는 어디서 보는가(이 주제 범위인가) | `wire-protocol` §7~§11 (범위 재검토 대상 — §4 S9 참조) |

## 3. 새 구조

`target-readme.ko.md`가 이미 파일 이름 초안을 잡아 두었다. 다만 그 표에 `05-transport-liveness`와
`05-wire-protocol`이 **번호가 겹치는 오타**로 적혀 있다 — 이 매핑표는 `06-wire-protocol`로 바로잡아
쓴다(§6에서 target-readme 수정 필요 항목으로 별도 기록).

```
spec/server/02-channel-transport/
  README.ko.md                     주제 진입 1장 — 개요, 책임 표, 물리/논리 그림 1개, 질문→문서 표
  01-channel-topology.ko.md        07 재작성
  02-channel-messaging.ko.md       08 재작성
  03-client-server-channel.ko.md   09 재작성
  04-network-listener-identity.ko.md  10 재작성 (§7 RID 정책 범위는 §4 S8에서 재검토)
  05-transport-liveness.ko.md      29 + 49 §1 병합 재작성 (§4 S9에서 병합 범위 확정)
  06-wire-protocol.ko.md           51 재작성
```

(en 짝 문서는 마지막 단계에 작성한다. §5.)

### 3.1 `channel-topology` 절 구성 (07)

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. RouteMesh topology 개요 — Application이 구성하는 값과 Framework가 만드는 결과 | 07 §1, 표 | 계약 |
| 2. MeshName과 MeshNode | 07 §3 | 계약 |
| 3. ChannelName role과 membership (Client/Server, 등록 interface) | 07 §4, §4.1 | 계약 |
| 4. Local Server 없이도 호출을 시작할 수 있는 경우 | 07 §4.2, §4.2.1(물리 그림), §4.2.2(논리 그림) | 계약 + 그림 2개(물리/논리, 가이드 §7.2) |
| 5. 실행 중 바꿀 수 있는 값(weight) | 07 §4.3 | 계약 |
| 6. 한 process에서 ChannelName은 한 송신 경로만 | 07 §4.4 | 계약 |
| 7. Channel handler를 구분하는 값 | 07 §4.5 | 계약 |
| 8. RouteMesh peer 연결과 discovery(automatic RID 비교, manual, descriptor 기반) | 07 §5, §5.1, §5.2 | 계약 |
| 9. Peer endpoint를 찾는 방법(automatic/manual, host Relocate 제약) | 07 §6 | 계약 |
| 10. Ready 상태와 Channel target 선택 | 07 §7 | 계약 |
| 11. RouteMesh SS message 크기와 mailbox 상한 | 07 §8 | 계약 |
| 12. Classic fanout과의 경계 | 07 §9 | 계약 |
| 13. 검증 요구 | 07 §10 — 인터페이스 관찰 문장으로 재구성 | 검증 |

### 3.2 `channel-messaging` 절 구성 (08)

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Node direct와 ChannelName select-one 개요, 공통 API 예시 | 08 §1, §2 | 계약 |
| 2. Target을 선택하는 방법 — Node direct | 08 §3.1 | 계약 |
| 3. Target을 선택하는 방법 — ChannelName select-one, 선택 순서(가중 라운드로빈) | 08 §3.2 | 계약 |
| 4. 등록되지 않은 ChannelName | 08 §3.3 | 계약 |
| 5. 선택 뒤 자동 재전송하지 않는 이유 | 08 §4 | 계약 |
| 6. Handler를 찾고 실행하는 방법(namespace, context 정보) | 08 §5, §5.1, §5.2 | 계약 |
| 7. Classic fanout과의 경계(liveness beacon topic 예약) | 08 §6, §6.1, §6.2 | 계약 |
| 8. 실패와 종료 | 08 §7 | 계약 |
| 9. Metadata와 관측 | 08 §8, §8.1, §8.2 | 계약 |
| 10. 검증 요구 | 08 §9 | 검증 |

### 3.3 `client-server-channel` 절 구성 (09)

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. ClientServer Channel 개요 — Client/Server role, RouteMesh와의 경계 | 09 §1 | 계약 |
| 2. Client와 Server role 등록 | 09 §3, §3.1 | 계약 |
| 3. Server endpoint를 찾고 ready로 만드는 방법 | 09 §4, §4.1~§4.4 | 계약 |
| 4. Weight와 target 선택(local Server 후보 포함) | 09 §5, §5.1, §5.2 | 계약 |
| 5. Send, request와 reply | 09 §6, §6.1, §6.2 | 계약 |
| 6. Drain | 09 §7 | 계약 |
| 7. Server 재시작 | 09 §8 | 계약 |
| 8. Location Store 장애 | 09 §9 | 계약 |
| 9. 검증 요구 | 09 §10 | 검증 |

### 3.4 `network-listener-identity` 절 구성 (10)

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Listener 주소 개요(bind/advertise), 공통 API 예시 | 10 §1, §2 | 계약 |
| 2. Process 기본값과 listener override, wildcard 규칙 | 10 §3, §3.1, §3.2 | 계약 |
| 3. Port를 확정하는 방법, publisher listener 상태 조회 | 10 §4, §4.1 | 계약 |
| 4. Listener 종류별 record(어디에 기록하고 어디에 기록하지 않는가) | 10 §5 | 계약 |
| 5. Listener 재시작과 lifecycle | 10 §6 | 계약 |
| 6. 시스템 전체 transport RID와 Spot ID 정책 | 10 §7, §7.1, §7.2, §7.3 | 계약 — **범위 재검토 대상, §4 S8** |
| 7. Kubernetes 배포 | 10 §8 | 계약 |
| 8. 검증 요구 | 10 §9 | 검증 |

### 3.5 `transport-liveness` 절 구성 (29 + 49 §1) — 병합 결정

**결론: 49 전체가 아니라 §1만 29에 병합한다.** §2~§5는 liveness가 아니라 서로 다른 세 주제(시작
순서·상태 공개, 구독 backpressure, 계측 비용)를 다루므로 이 문서로 가져오면 "29가 정의하는 것"의
범위를 벗어난 내용이 섞인다. 근거와 각 절의 실제 소속은 §4 S9에 정리한다.

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Application에서 보이는 결과 — 세 연결 방식, 확인 신호는 공개 API가 아님 | 29 §1 | 계약 |
| 2. 고정된 시간과 public API 경계(5초/15초, builder가 공개하지 않음) | 29 §2 + 49 §1 "확인 신호와 업무 message를 섞지 않는다" 요지를 이유 문장으로 흡수 | 계약 + **결정**(단일 기준, 설정으로 노출하지 않음 — 49 §1) |
| 3. RouteMesh와 ClientServer — probe/ack 절차, NotRequired 예외 | 29 §3 | 계약 |
| 4. Classic fanout — beacon, 수신 상한(3축, 값 미정) | 29 §4 | 계약 (미정 값은 검증 요구에 "값 미정" 명시) |
| 5. Ready와 장애 판정 | 29 §5 | 계약 |
| 6. Connection loss와 reconnect | 29 §6 | 계약 |
| 7. Location Store와 host 종료 | 29 §7 | 계약 |
| 8. Liveness 판정은 authority를 바꾸지 않는다(책임 분리) | 49 §1 "Liveness 판정은 authority를 변경하지 않는다" | **결정**(evidence만 게시, resolver·lifecycle·activation coordinator의 책임 분리) |
| 9. 관측 정보 | 29 §8 | 계약 |
| 10. 검증 요구 | 29 §9 + 49 §6 중 liveness 관련 8항목만 흡수 | 검증 |

### 3.6 `wire-protocol` 절 구성 (51)

절 제목·번호는 51 원문 그대로 유지한다(가이드 §4.4 wire 문서는 계약/구현 서술을 절로 나누기보다
방향만 표시할 수 있다고 허용하며, 51은 이미 "함께 보는 계약" 박스로 소유 문서를 밝혀 두었다).
재작성은 문장 층위 정리(결정 라벨 제거, 검증 요구를 인터페이스 관찰로)만 하고 절 순서·번호는
유지 — 이미 다른 문서 12개가 `#절-번호` anchor로 이 문서를 인용하기 때문이다(§6).

| 절 | 그대로 유지 | 처리 |
|---|---|---|
| 1~6 | Schema/생성 경계, framing, command space, admission, liveness wire, JSON profile | channel-transport 핵심 — 그대로 재작성 |
| 7~11 | durable authority, cold activation recovery, relocation manifest·CAS, membership/Ready, terminal identity | **범위가 spot-actor·location-relocation까지 걸침 — §4 S9에서 재검토 대상으로 남긴다.** 이번 재작성에서는 옮기지 않고 문장만 정리한다(가이드 §2.5 위반 없이) |
| 12 | 구현 검증 | 검증 절 형식만 §9.3에 맞춘다 |

## 4. 읽으면서 발견한 구조 문제 (재작성에서 처리)

| # | 문제 | 처리 |
|---|---|---|
| S1 | 07 §4.2.1과 §4.2.2가 물리/논리 그림을 이미 분리해 두었다(가이드 §7.2 요구를 이미 만족) | 그대로 유지, anchor(`#421-...`, `#422-...`)만 새 절 번호로 갱신 |
| S2 | weight 범위(`0..10000`, 기본 `100`)와 overflow-safe 합산 규칙이 07 §4.3, 08 §3.2, 09 §5에 **세 번** 반복 | `client-server-channel` §4가 ClientServer 쪽을, `channel-topology` §4.3이 RouteMesh 쪽을 소유하고, `channel-messaging` §3.2 "선택 순서"는 두 경로에 공통인 **가중 라운드로빈 알고리즘**만 소유한다. 세 문서가 서로 링크하고 값 자체는 한 곳(가중치 범위는 각 topology 문서, 알고리즘은 messaging 문서)에만 적는다 |
| S3 | Object Client가 RouteMesh Channel Server를 등록할 수 있다는 규칙과 Node direct handler 금지 규칙이 07 §4.2, §5.1과 08 §3.1에 **세 번** 반복 | `channel-topology` §4.2가 소유, `channel-messaging` §3.1은 결과(NotFound)만 인용하고 링크 |
| S4 | Manual RouteMesh의 `NotRequired` 판정 규칙이 07 §5.1, §6, 29 §3, §5, §6, 51 §5에 **여섯 번** 반복 | `transport-liveness` §5(Ready와 장애 판정)가 판정 기준을 소유. `channel-topology`는 등록 시점 결과만, `wire-protocol`은 wire 표현만 적고 나머지는 링크 |
| S5 | Local Server가 선택돼도 handler를 직접 호출하지 않고 실제 transport(DEALER→ROUTER)를 탄다는 문장이 07 §4.2.2, 08 §3.2, 09 §5.1에 **세 번** 거의 동일한 문장으로 반복 | `client-server-channel` §4가 ClientServer local-candidate 규칙을 소유(가장 상세). `channel-topology`·`channel-messaging`은 RouteMesh 쪽 결과 한 문장 + 링크 |
| S6 | 08 §3.2 "선택 순서"가 `###`(3레벨) 소제목으로 되어 있어 `##` 목록에서 빠짐 — 코드 주석이 이 절을 "§3.2"로 인용하는데 실제로는 절 번호가 안 붙는 소제목 | 재작성 시 정식 절 번호(`3.2 ChannelName select-one`과 그 하위 `가중 라운드로빈 선택 순서`)를 부여해 코드 주석이 가리키는 자리를 anchor로 고정 |
| S7 | 09 §1 표 제목이 영문 `Role \| ...`이 아니라 이미 한국어이지만, 09 전체에서 "즉시 확정" 류 결과 문장과 이유 문장이 한 문단에 섞여 있는 곳이 여러 곳(§5, §7, §8) | 가이드 §2.4대로 **굵은 규칙 + 이유** 불릿으로 분리 |
| S8 | **10 §7~§7.3(RID·Spot ID 발급 정책)이 "network listener identity"라는 문서 범위를 넘는다.** Core raw socket RID 형식, Framework 전역 diagnostic prefix, Entry Spot ID 발급까지 다뤄 listener 자체(bind/advertise 주소)와 직접 관련이 없는 시스템 전체 규칙이다 | 이번 주제에서는 그대로 옮기되(topic-map이 10 전체를 이 주제에 배정했으므로) 새 문서 §6에 "이 절은 listener 개별 설정이 아니라 시스템 전체 RID 발급 정책이다"라는 범위 문장을 추가한다. **재배치는 이번에 하지 않는다** — 00-foundation 주제(글로벌 식별자 정책)로 옮길지는 그 주제 작업 때 판단하도록 spec-gap 후보로 남긴다(§7 spec-gap 후보 G1 아님, 구조 후보로 별도 기록) |
| S9 | **49 전체가 29에 병합되지 않는다.** §1(liveness 단일 기준·authority 비침범)만 liveness다. §2~§3(시작 순서·상태 값 7개·`serving` 공개 순서)은 [13-mesh-node](../../../../../framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md) §6과 [24-runtime-monitoring](../../../../../framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md) §2.1·§2.2가 이미 소유한 계약의 구현 서술이고, §4(느린 구독자 backpressure·합치기)는 24의 "합치기" 절이 소유, §5(trace 비용)는 [26-message-flow-tracing](../../../../../framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md) §4.1이 소유한다. 이 세 절은 **channel-transport 주제가 아니라 03-spot-actor(13)·06-observability(24, 26) 주제 소관**이다 | `transport-liveness`에는 49 §1만 병합한다(§3.5). §2~§5는 이번 재작성에서 옮기지 않고 원문 그대로 49 파일에 남긴다 — 49 문서 자체를 아직 폐기하지 않는다(다른 주제가 아직 시작하지 않았으므로). **topic-map.ko.md의 "29(+49)" 표기는 부정확하다** — "29(+49 §1)"로 정정이 필요하며, 49 §2~§5의 최종 배치는 03·06 주제가 시작될 때 결정한다. 이 매핑표에서는 결정만 내리고 topic-map.ko.md는 고치지 않는다(작업 범위 — 산출물 파일 1개 외 수정 금지) |
| S10 | **51 §7~§11(전체의 절반 이상, 약 400줄)이 durable authority·cold activation recovery·relocation manifest·CAS·membership Ready·terminal identity를 다뤄 "service wire protocol"보다 훨씬 넓다.** 이 내용은 21-location-runtime(05-location-relocation 주제), 28-relocation-flow, 52-internal-relocation-handoff, 15-spot-actor(03-spot-actor 주제)와 직접 겹친다 | topic-map이 51 전체를 이 주제의 독립 문서로 배정했으므로 이번에는 그대로 유지한다. 문장 정리만 하고 절 재배치는 하지 않는다. §3.6과 마찬가지로 재배치 여부는 05-location-relocation·03-spot-actor 주제가 시작될 때 코디네이터가 판단할 구조 후보로 남긴다 |
| S11 | 07·08·09가 각각 "실패와 종료" 표를 갖고 있는데 열 이름과 형식이 다르다(07은 검증 요구에 산문, 08 §7은 `조건 \| 결과` 표, 09 §10은 검증 요구 불릿) | 가이드 §9.3대로 세 문서 모두 검증 요구 절을 "공개 표면 → 관찰 결과" 한 문장 불릿으로 통일. 표는 §9.3 이전 절(정상/실패 설명)에서만 쓴다 |
| S12 | 10 §1 "Network listener에는 서로 다른 목적의 주소가 두 개 필요할 수 있다"는 이미 결과로 시작해 메타 제목 문제(S6/S7 session 사례)가 없다 — 문제 없음, 유지 | 처리 불필요 |
| S13 | 29·49·51에 "확인 신호는 application에 도달하지 않는다"는 문장이 **세 번**(29 §1·§3, 49 §1, 51 §5) 거의 동일하게 반복 | `transport-liveness` §1이 소유(공개 계약), `wire-protocol` §5는 wire 표현(어느 command가 infra reserve로 가는지)만 적고 링크 |

## 5. 규칙 등가성 대장

재작성 뒤 이 표의 모든 행이 새 문서의 정확히 한 절에 있어야 한다("새 위치" 열은 재작성 뒤 채운다).
행이 없으면 누락, 표에 없는 보장이 새 문서에 있으면 추가 보장이며 둘 다 대조 실패로 본다.
소스 문서별로 그룹화했다.

### 5.1 `07-channel-topology`

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R1 | MeshName이 RouteMesh 참여 범위와 routing ID 유일성 범위를 정함; 다른 MeshName은 자동 중계되지 않음; 같은 process의 application은 서로 다른 RouteMesh로 각각 새 호출을 시작할 수 있음(중계 아님) | §3 | |
| R2 | 같은 process에 같은 MeshName MeshNode는 하나만; 다른 MeshName은 여러 개 등록 가능 | §3 | |
| R3 | MeshNode 값 3개(MeshName, Routing ID, ROUTER endpoint); descriptor에 MeshName·RID·lifecycle generation·descriptor revision·실제 endpoint·Server ChannelName set·weight 포함; RID·MeshName·endpoint identity는 동작 중 불변 | §3 | |
| R4 | ChannelName role: `Client`(송신 경로만 등록, target으로 미게시) / `Server`(송신 경로+target membership 게시, handler·weight 제공) | §4 | |
| R5 | 같은 ChannelName에 Client와 Server를 동시에 등록하지 않음; `SetWeight(0)`은 Server를 Client로 바꾸지 않음(membership 유지, 선택에서만 제외) | §4.1 | |
| R6 | Object Client는 RouteMesh Channel Server 등록 가능하나 application Node direct handler는 등록 불가(조합 시 startup configuration error); Object Server는 두 role 모두 사용 가능 | §4.2 | |
| R7 | Local Server role 없이도 Client role만으로 Channel 호출 시작 가능; Client/Server 모두 같은 ROUTER·peer 연결 공유, 별도 socket 아님 | §4.2 | |
| R8 | 아무 role도 없는 MeshNode도 Node direct는 시작 가능(target 게시는 없음); role별 시작 가능 작업·게시 여부 3행 표 | §4.2 | |
| R9 | Client role만 등록한 MeshNode descriptor는 Server ChannelName set을 빈 값으로 게시; 가짜 Server membership이나 weight 0 membership을 startup에 요구하지 않음 | §4.2 | |
| R10 | 물리 연결 그림: 두 MeshNode가 모두 Object Client이고 양쪽 모두 RouteMesh Channel Server membership이 없을 때만 peer 연결 생략; MeshName이 다르면 자동 연결 안 됨; Object Client에는 Node direct target 없음(선택으로 바뀌지 않음, target 없음으로 종료) | §4.2.1 | |
| R11 | ChannelName은 새 socket·peer 연결을 만들지 않음; select-one은 같은 MeshName의 ready+weight>0 Server 중 하나(multicast 아님); Server role 등록 caller도 자기 호출의 선택 후보(ready·weight>0·non-draining이면) | §4.2.2 | |
| R12 | Remote Server 선택 시 §4.2.1 기존 peer 연결 사용, 자기 node 선택 시 같은 처리 경로로 local submission(codec·admission·HWM·timeout·correlation·terminal completion 생략 없음, handler 직접 호출 우회 없음); positive weight 후보 없으면 target 없음 | §4.2.2 | |
| R13 | Client/Server role 목록은 startup 뒤 불변; weight만 `0..10000`(기본 `100`) 실행 중 변경 가능, 범위 밖은 startup·runtime 모두 configuration error | §4.3 | |
| R14 | readiness·capacity·drain 적용 뒤 남은 positive weight 합을 최소 64-bit 정수로 계산, overflow 방지 비율만 사용 | §4.3 | |
| R15 | weight 변경은 이후 시작하는 select-one·Logical Multicast에만 적용, 이미 제출한 작업·RID direct·다른 ChannelName membership에는 영향 없음 | §4.3 | |
| R16 | descriptor revision·endpoint는 public status에 미포함(내부 stale 판정용); monitoring은 실제 선택된 MeshName·RID만 제공 | §4.3 | |
| R17 | ChannelName은 한 process에서 하나의 물리 송신 경로만 가리킴; 서로 다른 topology(RouteMesh/ClientServer/fanout)에 같은 이름 등록 시 startup 실패 | §4.4 | |
| R18 | ClientServer registration key = `(ChannelName, Role)`; 같은 process에 Client 1회+Server 1회는 하나의 topology로 병합, 같은 role 중복은 startup 실패 | §4.4 | |
| R19 | Runtime은 현재 process 등록만 검사(전역 catalog 불필요); topology 이동 시 이전·새 경로 동시 등록 금지, live migration·message 중계·pending request 이전 미제공 | §4.4 | |
| R20 | Channel handler 구분 값 = `(ChannelName, message kind, packet identity)`; MeshName 미제공; Node direct handler는 MeshName+RID 별도 route 범위라 같은 packet name 충돌 없음 | §4.5 | |
| R21 | 같은 MeshName의 ready MeshNode는 pair마다 직접 연결, node N개면 peer 연결 최대 N-1개; 양쪽 Object Client이고 RouteMesh Channel Server membership도 없는 pair는 제외 | §5 | |
| R22 | Automatic: 두 role 모두 Object Client이면 connection intent 미생성; 그 외 pair는 RID canonical byte order가 더 작은 MeshNode만 연결 시작 | §5.1 | |
| R23 | Manual: 양쪽 endpoint 등록 가능, 동시 시작 시 handshake·admission으로 RID·lifecycle generation 중복 연결을 하나만 ready 유지 | §5.1 | |
| R24 | Manual에서 handshake로 양쪽 Object Client 확인되면 "연결 불필요" terminal admission 기록 후 ready 전 close; 같은 endpoint·configuration generation은 재시도 안 함; endpoint·expected RID·generation 변경 시 새 intent로 재확인 가능 | §5.1 | |
| R25 | descriptor 기반 연결: endpoint만으로는 generation `0`·RID를 security identity로 쓰는 fallback 사용 안 함; descriptor 못 찾으면 descriptor 기반 placement 주장 안 함; descriptor 찾을 때까지 endpoint-only intent 유지, 교체는 이전 intent가 liveness close로 닫히기 전에는 설치 안 함 | §5.1 | |
| R26 | peer handshake 확인 정보 7항목 표(MeshName·RID, lifecycle generation, descriptor revision, Object role, Server ChannelName set·weight, endpoint·security identity, protocol version·capability) | §5.1 | |
| R27 | MeshName·trust profile 다르거나 같은 lifecycle identity에서 RID 충돌 시 ready 안 됨; lifecycle generation은 값이 같은지만 비교(크기 비교 안 함) | §5.1 | |
| R28 | Fixed RID manual topology의 재연결 조건 3가지(재연결 의도 명시, identity·security 확인, 이전 연결 종료를 liveness로 확인); 늦게 도착한 이전 generation frame·event는 현재 연결에 영향 없음 | §5.1 | |
| R29 | Descriptor revision은 같은 lifecycle 안에서 1 이상 증가; owner가 weight 바꾸면 revision 증가 → Store·연결된 peer에 게시 → peer는 같은 lifecycle의 더 큰 revision만 적용 → target 목록 한 번에 교체; update 유실 시 다음 polling·handshake에서 재확인; weight 변경만으로 연결 재생성 안 함 | §5.2 | |
| R30 | Peer endpoint 발견 방식 2가지(automatic: Redis Location Store, 명시 등록 필요 / manual: application 등록, peer 연결만이면 Store 불필요); manual도 같은 handshake·중복 제거 규칙 사용; expected RID 지정 시 불일치하면 연결 실패, 생략 시 handshake 결과로 identity 확정 | §6 | |
| R31 | Manual peer 양쪽 Object Client이고 RouteMesh Channel Server membership 없으면 host 중단 없이 해당 intent만 `NotRequired` terminal로 끝나 ready·liveness 대상 제외, 재시도 안 함; monitoring은 `NotRequired`를 `NotConnected`와 구분 | §6 | |
| R32 | Manual mode는 일반 messaging·`Shutdown`에는 쓸 수 있으나 host `Relocate` 무중단 handoff에는 사용 불가; manual connection이 하나라도 있으면 `Relocate`는 상태·admission 변경 전 `Blocked/ManualTopologyUnsupported`로 끝남 | §6 | |
| R33 | Manual peer 연결과 Spot·Actor 위치 조회는 별개 기능; manual endpoint를 쓰더라도 분산 Spot·Actor 주소·relocation을 쓰면 Redis Location Store 필요 | §6 | |
| R34 | MeshNode ready 조건 4가지(ROUTER listener bind, peer 연결 admission 준비, local handler 등록, Spot·Actor 등록); 연결할 peer가 없어도 ready 전환은 막지 않음 | §7 | |
| R35 | ChannelName Server는 MeshNode ready이고 자기 weight>0일 때만 새 select-one target; 후보는 descriptor에 게시된 membership만; target 선택·submit은 하나의 작업, 선택 RID를 중간 결과로 반환 안 함 | §7 | |
| R36 | Client role은 remote Server 선택 수에 미포함(local 송신 경로만); drain 시작한 MeshNode는 새 ChannelName 선택·Logical Multicast target에서 제외 | §7 | |
| R37 | RouteMesh SS에는 Framework-level `MaxMessageSize` 없음; `SendHighWaterMark`·`ReceiveHighWaterMark`·`SendTimeout`·`ReceiveTimeout`은 방향별 socket option; `MailboxMessageBudget`·`MailboxByteBudget`은 socket HWM과 별개인 owner별 mailbox 상한 | §8 | |
| R38 | 하위 transport·protocol 표현 한계에서 거부되면 payload 일부를 handler에 전달하지 않고 오류 모델 terminal로 끝남; application이 이 하위 한계를 payload 검사로 대신하지 않음 | §8 | |
| R39 | ClientServer는 Framework API §6의 일반 listener `MaxMessageSize` 계약 유지; StreamNode 64 KiB 기본값·Core STREAM 방향별 규칙은 ClientServer에 적용 안 함 | §8 | |
| R40 | Classic fanout은 독립 PUB/SUB socket 사용, RouteMesh full mesh·membership에 미참여, MeshNode 불필요; RouteMesh Channel과 별도 계약(endpoint·전달 정책·monitoring 독립, 서로 해석 안 함) | §9 | |
| R41 | Automatic fanout subscriber는 같은 ChannelName publisher descriptor만 조회(MeshNode·ClientServer descriptor 미사용); publisher RID·lifecycle generation 조합마다 connection intent 1개, publisher끼리·subscriber끼리는 연결 안 함 | §9 | |
| R42 | Automatic publisher는 bind 뒤 lifecycle별 RID·endpoint를 descriptor에 게시만, subscriber 발견·outbound connect 시작 안 함 | §9 | |
| R43 | Manual subscriber는 Location Store descriptor 미사용; 같은 fanout ChannelName에 automatic·manual subscriber를 동시 설정하면 startup 실패, 자동 합침·fallback 없음 | §9 | |
| R44 | Subscriber는 automatic이면 publisher descriptor마다, manual이면 endpoint마다 전용 SUB socket 1개; 여러 publisher를 한 socket에 함께 연결 안 함(source identity 없어 activity·timeout 구분 불가하기 때문) | §9 | |

### 5.2 `08-channel-messaging`

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R45 | Node direct: `MeshName`+target RID 지정, 다른 RID로 안 바꿈; ChannelName select-one: `ChannelName` 하나 지정, 현재 process 등록 경로에서 RouteMesh는 ready Server membership 중, ClientServer는 ready server 중 선택 | §1 | |
| R46 | Node direct target 조건 4가지(같은 MeshName 참여, 지정 RID 일치, Object role≠Client, ready); 불일치·timeout 시 target 오류/timeout, 다른 RID로 자동 전환 안 함 | §3.1 | |
| R47 | Object Client는 Node direct target 아님, `NotFound`로 종료(Client pair connection 생성 안 함); 같은 MeshNode의 RouteMesh Channel Server 등록에는 이 제한 미적용 | §3.1 | |
| R48 | ChannelName select-one 5단계 절차(경로 확인 → RouteMesh는 Server membership만/ClientServer는 ready server만 후보 → weight 0·drain 제외 → weight 비율 반영 선택+즉시 submit) | §3.2 | |
| R49 | 선택 순서(가중 라운드로빈): 후보마다 누적값(초기 0) 유지 → 매 선택 시 전체 후보 누적값에 자기 weight 더함 → 누적값 최댓값 후보 선택(동률이면 후보 식별자 오름차순 — RouteMesh는 NodeRid, ClientServer는 Server RID, byte 열 부호 없는 비교, 짧은 쪽이 접두사면 짧은 쪽 우선) → 선택된 후보 누적값에서 전체 weight 합 뺌; 누적값은 송신 경로가 유지, 후보 목록 변경 시 새 목록에 없는 항목 폐기 | §3.2 | |
| R50 | 같은 후보 목록·같은 누적값 상태에서는 항상 같은 순서(application이 재현성에 의존 가능); weight 100·300 두 후보는 장기 비율 1:3을 유지하며 연속 선택이 한쪽에 몰리지 않음(예시 순서 `B,A,B,B`) | §3.2 | |
| R51 | ClientServer local Server도 remote와 같은 후보(listener bind+service admission 마쳐 ready, weight>0, non-draining일 때만); local이라는 이유로 우선 선택·제외 안 함; 선택돼도 handler 직접 호출 안 하고 DEALER→ROUTER 실제 전송(codec·admission·HWM·timeout·correlation·reply 우회 없음) | §3.2 | |
| R52 | 이 local 후보 규칙은 ClientServer 전용; RouteMesh는 보내는 MeshNode 자신의 Server role이 후보에 안 들어감(peer 연결 없음, descriptor 게시 membership만 후보); 자기만 Server인 MeshNode에서 RouteMesh select-one 호출 시 후보 없음 → target 없음 실패, 같은 process 처리는 ClientServer 경로 사용 | §3.2 | |
| R53 | 후보 없을 때: RouteMesh는 즉시 target 없음 실패; ClientServer는 `min(request timeout, 5초)` 동안 대기 후 실패(진행 중인 admission이 끝나기를 기다릴 뿐, admission을 유발하지 않음); Framework startup은 local ClientServer admission 완료를 안 기다림 | §3.2 | |
| R54 | 등록 안 된 ChannelName 호출은 다른 MeshNode·ClientServer client로 자동 검색·전송하지 않음(현재 호출의 자동 fallback에만 적용); application은 다른 등록된 ChannelName이나 새 Node direct 호출을 별도로 시작할 수 있으나 원래 호출의 target·경로는 안 바뀜; 같은 ChannelName을 물리 송신 경로 둘 이상에 등록하면 startup 실패(07 R17과 중복 언급) | §3.3 | |
| R55 | target 선택+submit 뒤 연결 종료·timeout 발생해도 다른 Server member에 자동 재전송 안 함(첫 target이 이미 실행했을 수 있어 중복 실행 위험); application이 실패 후 명시적으로 새 request 시작해야 하며 이는 자동 재전송이 아닌 별도 operation | §4 | |
| R56 | Node direct·ChannelName handler는 서로 다른 handler namespace(Node direct: MeshName+kind+packet name / ChannelName: ChannelName+kind+packet name); 같은 handler 범위에서 (kind, packet name) 중복 등록 시 startup 실패, 다른 범위끼리는 packet name 재사용 가능 | §5 | |
| R57 | Channel handler context 제공 값 4개(ChannelName, kind·packet name, metadata, request correlation), MeshName 미제공; Node direct handler context는 MeshName·source node RID 추가 제공 | §5.1 | |
| R58 | Node direct·Spot direct·Actor direct는 서로 다른 주소 지정 방식, handler 안 섞임; Framework는 payload type·내용으로 자동 변환 안 함(전용 API로 명시 지정 필요); reply route·correlation은 Framework가 보존, handler가 직접 만들지 않음 | §5.2 | |
| R59 | Classic fanout 미제공 기능 4개(durable 저장, ack, replay, 손실 없는 전달); 수신 queue HWM 도달 시 해당 subscriber에만 전달 버림, publish는 성공, 다른 subscriber는 영향 없음(느린 subscriber가 publisher를 막지 않음) | §6 | |
| R60 | Framework 연결 상태 확인용 liveness beacon topic = `01 5A 4C 46 31`(5 byte 정확히); public publish API가 이 값과 정확히 같은 topic 사용 불가(호출 인자 오류); 길이·byte 다르면 사용 가능(4개 예시) | §6.1 | |
| R61 | Subscriber는 이 topic 신호를 application event로 미처리(등록 handler 미실행, message-flow 관측에도 미게시); byte 형식·판단 시간 기준은 `Transport liveness`가 정의(링크) | §6.1 | |
| R62 | Publisher에는 `IZLinkFanoutPublishCall`이 있고 `Async`는 local publisher transport 수락만 기다림(subscriber handler 완료 안 기다림); Topic 생략 시 packet name을 topic으로 사용; metadata setter 없음(이 문서의 Node direct·ChannelName application metadata 계약 미적용) | §6.2 | |
| R63 | 실패 표 8행(Node direct target 미참여→target 못 찾음 오류; Object role Client→NotFound, Client pair 미생성; 미등록 ChannelName→NotFound 다른 경로 전송 안 함; 선택 가능 target 없음→NotFound; ready timeout→route 오류/timeout; request handler 못 찾음/해석 실패→reply 경로 있으면 error reply; one-way handler 못 찾음/해석 실패→handler 미전달·runtime 관측 기록; host 신규 submit 미수락→해당 언어 shutdown 오류) | §7 | |
| R64 | ChannelName 호출은 drain 중인 member를 선택 후보에서 제외; Node direct는 caller 지정 RID를 유지(drain 상태여도 다른 RID로 안 바꿈, 지정 node의 연결·수락 상태에 성공 여부가 달림) | §7 | |
| R65 | ClientServer client 제출 request와 불일치하는 server message는 `ProtocolError`로 기록, application handler 미전달; 각 service runtime은 transport 전용 오류를 공통 Framework 결과로 변환(transport library 내부 result를 public call에 직접 노출 안 함) | §7 | |
| R66 | Application metadata 계약: key/value는 UTF-8, NUL 미포함; 전체 크기 최대 1024 bytes(encoding+구조 overhead 포함); 같은 key 재설정 시 마지막 값 전송; handler는 불변 snapshot으로 읽음(turn 이후 보관하려면 application이 복사); 잘못된 metadata는 handler 미실행, protocol 오류; reply에는 metadata setter 없음, request metadata 자동 복사 안 됨 | §8.1 | |
| R67 | Handler가 다른 target으로 새 request 시작해도 현재 request의 metadata 자동 복사 안 됨(application이 새 call에 명시 설정); Framework가 자동 전파하는 trace 상관관계는 application metadata와 별도 Framework field | §8.1 | |
| R68 | Metadata frame 배치·encoding은 공개 계약 아님(application이 직접 만들거나 해석 안 함) | §8.1 | |
| R69 | 관측 정보가 구분해야 하는 값 7항목(ChannelName, 송신 경로 종류, MeshName, source·target RID/server identity, 선택 결과와 송신 경로 수락 여부, handler 전달 결과, drain state); 물리 식별자는 handler context에 미추가, 고cardinality 업무 식별자는 metric label 미사용 | §8.2 | |

### 5.3 `09-client-server-channel`

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R70 | Client role: ready server 1개 선택해 send/request 시작, Server가 먼저 보낸 message 미수신, 자기 request와 일치하는 reply만 수신; Server role: client 대상 새 업무 호출 시작 불가, client의 send·request handler 실행, request handler는 reply token으로 reply | §1 | |
| R71 | ClientServer Channel이 제공하지 않는 기능 4개(Node direct, Spot·Actor 메시징, Logical Multicast, 다른 RouteMesh 대신 연결·중계); Server가 알림·event를 먼저 보내려면 별도 RouteMesh 등록 필요 | §1 | |
| R72 | Registration key = `(ChannelName, Role)`; Client·Server 역할별 최대 1회; ChannelName+Server만 있고 Client 없으면 local handler 직접 호출 안 하고 `NotConfigured`로 끝남(`NotFound`는 ChannelName·target 자체가 없는 경우) | §3 | |
| R73 | startup configuration error 3가지(같은 ChannelName Client 중복 등록, 같은 ChannelName Server 중복 등록, 같은 ChannelName을 RouteMesh와 ClientServer에 동시 등록); 서로 다른 ClientServer ChannelName은 여러 개 등록 가능 | §3.1 | |
| R74 | 같은 process에서 동일 ChannelName의 Client 1회+Server 1회는 정상, 하나의 topology로 합쳐짐; 같은 ChannelName의 Server는 여러 process에 등록 가능; 식별 정보 불일치 message는 protocol 오류로 기록(handler 미실행, 다른 request의 reply로 미사용) | §3.1 | |
| R75 | Client는 manual(`Connect(endpoint)`)·automatic(같은 ChannelName Server descriptor 조회) 두 source에서 endpoint 획득; 같은 Server RID·lifecycle generation을 가리키면 연결 후보 하나로 합침; Location Store는 automatic에만 필요 | §4 | |
| R76 | Client만 connection 시작, Server는 Client endpoint 탐색·outbound connection 시작 안 함(RouteMesh와 달리 RID 비교로 시작 쪽 정하지 않는 비대칭 topology); automatic discovery는 (Server RID, lifecycle generation)마다 connection intent 1개, 여러 Server 발견 시 각 ready connection 독립 유지 | §4.1 | |
| R77 | ClientServer Server descriptor 필드 5개(ChannelName, Server identity·lifecycle generation, endpoint, weight·drain state, descriptor revision) + owner lease; client는 endpoint 찾은 뒤 실제 transport 연결에서 identity·generation 재확인해야 ready; descriptor에 MeshName·RouteMesh membership·Spot/Actor location·MeshNode peer 수락 정보 미포함(MeshNode descriptor와 상호 미사용) | §4.3 | |
| R78 | Manual endpoint만 쓰면 Location Store 불필요; automatic discovery 활성화했는데 Store 없으면 Server listener bind 전 startup 실패; manual connection도 ChannelName·Server RID·generation·weight·drain·security identity를 실제 연결에서 확인(MeshNode descriptor·RouteMesh peer 정보로 변환 안 함) | §4.4 | |
| R79 | Server weight `0..10000`, 기본 `100`, 범위 밖은 startup·runtime 모두 configuration error; weight는 처리 용량이 아니라 상대 비중(예: 100 vs 50 → 2배 비중, 개별 request 보장 아님); 같은 weight끼리는 순환 선택 | §5 | |
| R80 | Ready+non-draining Server 사이에서만 weight 비교, positive weight 합을 최소 64-bit로 계산해 overflow 방지; weight `0`(실행 유지, 선택만 제외, 다시 올리면 후보 복귀)과 drain(작업 정리 후 종료)의 의미 구분 | §5 | |
| R81 | Local Server도 remote와 같은 후보(listener bind+admission 완료 ready, weight>0, non-draining); local이라는 이유로 우선/제외 안 함; 선택돼도 handler 직접 호출 안 하고 DEALER→ROUTER 실제 transport message(codec·HWM·timeout·cancellation·request-reply 식별·terminal completion 우회 없음) | §5.1 | |
| R82 | target 선택+submit은 하나의 작업(중간 결과로 identity 반환 안 함); submit 뒤 연결 종료·timeout·cancellation 발생해도 다른 Server에 자동 재전송 안 함(첫 Server가 이미 실행했을 수 있음) | §5.1 | |
| R83 | 실행 중 weight·drain 변경 시 descriptor revision 증가, client는 같은 lifecycle generation의 더 큰 revision만 적용(늦게 온 이전 descriptor로 되돌아가지 않음); local weight 변경은 ChannelName으로 지정(Server RID·endpoint는 monitoring 전용, 변경 target 아님) | §5.2 | |
| R84 | Send = ready Server 1개에 one-way, reply token 미생성; Request는 ready Server 선택 후 reply correlation 생성, reply/error/timeout/cancellation/shutdown 중 먼저 확정된 결과로 완료 | §6 | |
| R85 | Reply token은 현재 request에만 사용, 최종 reply 후 재사용 불가; reply 경로 복원 가능한 실패 3가지(handler 못 찾음, payload 해석 불가, handler 예외)는 구조화된 error reply로 완료; one-way의 같은 실패는 reply 미생성, handler 미전달, runtime 관측 정보에 기록 | §6.1 | |
| R86 | ClientServer handler가 다른 RouteMesh·ClientServer Channel·Spot·Actor에 downstream request 가능, 별도 요청·응답 연결 정보 사용; 원래 request는 handler가 반환한 reply로만 한 번 완료(downstream reply의 연결 정보로 원래 값을 안 바꿈) | §6.2 | |
| R87 | Server drain 4단계(local ready 닫고 신규 수락 중단 → descriptor에 draining state+더 큰 revision 게시 → 이미 수락한 handler·reply를 deadline까지 진행 → 결과 확정 후 descriptor·owner lease 해제, listener 닫음); manual client에는 연결 control message로 drain state 통지; drain 확인 직전 제출된 request는 유한한 rejected 결과로 완료(무한 대기 없음) | §7 | |
| R88 | Server 재시작 시 새 lifecycle generation 발급(숫자 크기로 순서 판단 안 함), 같은 endpoint여도 이전 generation 연결·descriptor를 새 target으로 미사용; client 교체 4단계(새 generation descriptor 탐색 → transport 연결에서 identity·generation 재확인 → 새 generation을 ready target으로 → 이전 generation 연결 제거) | §8 | |
| R89 | client는 reply의 correlation을 현재 대기 중인 request 값과 비교, 일치할 때만 그 request 결과로 처리(이전 generation reply라도 원래 request가 대기 중이면 결과가 될 수 있음); timeout·cancellation·재시작으로 correlation 사라지면 늦은 reply 폐기, 이후 다른 request 결과로 미사용 | §8 | |
| R90 | Location Store 장애 시 client는 마지막 성공 automatic 후보 유지, 새 descriptor 추가·제거 계산 중단; 이미 ready인 연결·이미 수락한 request는 장애만으로 취소 안 됨; owner lease 갱신 실패 후 허용 시간(`fencing deadline`) 지나면 신규 업무 미수락; Store 복구 시 최신 revision·generation으로 target 목록 재정렬 | §9 | |

### 5.4 `10-network-listener-identity`

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R91 | Listener 주소 2종(Bind: local interface·port / Advertised: remote가 실제 접속할 host·확정 port); RouteMesh·ClientServer·fanout publisher·STREAM server는 process 기본 network 값 공유, 특정 listener만 override 가능; HTTP listener는 별도 URL 계약이라 이 문서의 자동 Location Store record 대상 아님 | §1 | |
| R92 | listener별 `SetBindHost`·`SetAdvertiseHost`는 그 listener에만 적용, 미지정 시 process 기본값 사용; 한 RouteMesh listener의 override가 다른 listener의 endpoint를 안 바꿈 | §3 | |
| R93 | Process 기본 BindHost = `127.0.0.1`; BindHost가 wildcard가 아니고 AdvertiseHost 생략 시 같은 host를 사용(local 환경용); container·다중 host는 AdvertiseHost 명시 필요 | §3.1 | |
| R94 | `0.0.0.0`·`::`는 wildcard, BindHost만 허용, AdvertiseHost 미허용; BindHost가 wildcard이면 AdvertiseHost 필수(없으면 endpoint·discovery record 게시 전 startup 실패) | §3.2 | |
| R95 | Port `0`이면 OS가 빈 port 선택, Framework가 실제 bound port 읽음; bind endpoint = BindHost+설정/할당 port, advertised endpoint = AdvertiseHost+실제 bound port; automatic discovery listener는 port 생략 시 `0` 사용 | §4 | |
| R96 | Manual mode에서 별도 discovery source 없으면 server listen endpoint·client remote endpoint 모두 명시 필요; wildcard host·port `0`은 local bind 입력에만 사용 가능, advertised endpoint·Location Store record·manual peer 설정에 남으면 startup 설정 오류 | §4 | |
| R97 | Publisher listener 상태 조회는 host 시작+listener bind 완료 뒤에만 성공, 반환 port는 설정값이 아니라 OS 실제 선택 port; 결과 endpoint = AdvertiseHost+실제 bound port(AdvertiseHost 미지정 시 bind endpoint); remote publisher descriptor의 내부 generation·discovery 상태는 미노출; application은 이 값을 subscriber 설정에 미복사(관찰 자료로만 사용) | §4.1 | |
| R98 | 공통 listener 상태 조회는 종류+설정 이름으로 지정, 결과에 종류·이름·advertised endpoint·조회 시각 포함; bind 안 끝난·알 수 없는 listener는 configuration error; 4종(`ROUTE_MESH`, `CLIENT_SERVER`, `FANOUT`, `STREAM`) 모두 같은 의미 제공 | §4.1 | |
| R99 | Listener 종류별 기록 위치 4행 표(RouteMesh MeshNode→MeshNode descriptor, ClientServer Server→ClientServer Server descriptor, Classic fanout publisher→fanout publisher descriptor, STREAM server→명시 설정/STREAM 자체 discovery 계약) — 각각 다른 descriptor 종류에는 기록 금지 | §5 | |
| R100 | Automatic fanout publisher는 fanout publisher descriptor를 Location Store에 게시, subscriber는 같은 ChannelName descriptor만 조회; Store 미사용 publisher는 descriptor 미게시(manual subscriber용 고정 endpoint를 application 설정으로 제공); STREAM endpoint는 Location Store에 자동 게시 안 됨 | §5 | |
| R101 | AdvertiseHost·실제 bound port가 바뀐 listener 재시작 시 새 endpoint+새 lifecycle generation을 같은 descriptor revision 체계에 기록; endpoint만 바꾸며 이전 generation 유지 안 함; remote runtime은 descriptor identity·generation이 실제 연결 값과 같은지 확인 후 ready 취급 | §6 | |
| R102 | 같은 process의 RouteMesh·ClientServer·fanout listener는 각자 독립 descriptor·lifecycle generation을 가짐; 한 listener의 endpoint 변경을 다른 topology의 generation 변경으로 해석 안 함 | §6 | |
| R103 | Core에는 별도 `NID` type 없음; Routing ID = 1..255-byte binary-safe opaque value; `Node RID`는 MeshNode 식별용 Routing ID의 역할명; application·provider는 RID 문자열 형식을 routing·placement·owner 관계 계산 입력으로 미사용 | §7 | |
| R104 | Core raw socket automatic RID = RFC 4122 UUID v4 16-byte binary(caller 미지정 시); Framework automatic RID도 UUID v4 random identity; diagnostic prefix가 필요한 topology는 36자리 lowercase canonical 문자열+prefix 사용; MeshNode 등 transport identity는 완성된 UTF-8 RID를 Core socket에 명시 설정; Entry Spot 등 logical identity는 Core socket 미설정, descriptor·Location Store authority에 기록 | §7 | |
| R105 | RID·identity 발급·표현 6행 표(Core raw socket automatic RID, Framework prefix RID `<prefix>-<uuid-v4>`, Entry Spot ID `<prefix>-entry-<uuid-v4>`, caller-지정 fixed transport RID, caller-지정 User·Instance Spot ID `UTF-8 1..255 bytes`, STREAM connection RID `Core STREAM 4-byte`) | §7 | |
| R106 | Diagnostic prefix 제한(ASCII `A-Z a-z 0-9 . _ -`만, 길이 `1..64`); UUID는 RFC 4122 v4를 `8-4-4-4-12` 36자 lowercase canonical로 표현; Full RID = `prefix-<uuid-v4>`, UTF-8 255 bytes 이하; prefix·UUID는 진단 정보(application identity·object placement·shard·재시작 후 유지되는 stable host 이름 아님) | §7.1 | |
| R107 | MeshNode descriptor 게시 시 Location Store가 `(MeshName, RID)` 사용 중 여부 확인; active conflict 발견 시 기존 descriptor 유지·새 claim 시도 없이 즉시 startup configuration error; replacement MeshNode는 endpoint 같아도 새 lifecycle+새 UUID RID 사용(UUID는 lifecycle generation을 대체하지 않음); Fixed RID는 automatic discovery 미사용 명시 manual topology에서만 허용, automatic discovery와 함께 설정 불가 | §7.2 | |
| R108 | Object Server MeshNode 시작 시 같은 diagnostic prefix를 쓰는 Entry Spot ID를 별도 발급(`<prefix>-entry-<entry-uuid-v4>`); MeshNode·Entry Spot은 각각 별도 UUID 생성(두 UUID가 다르다는 사실을 관계 판정 근거로 미사용); Full Entry Spot ID ≤255 UTF-8 bytes; 같은 MeshNode lifecycle은 같은 Entry Spot ID 유지, replacement lifecycle은 새 UUID 기반 Spot ID 발급 | §7.3 | |
| R109 | global Spot ID authority active conflict 확인 시 새 UUID·reservation 재시도 없이 즉시 startup configuration error; MeshNode descriptor는 lifecycle generation과 exact Entry Spot ID의 mapping 게시(Actor placement·Entry Spot join·relocation은 이 mapping 사용, 문자열 parsing 안 함) | §7.3 | |
| R110 | `<prefix>-entry-<uuid-v4>` 형식은 Framework 발급 Entry Spot identity 전용 예약; caller가 User·Instance Spot ID로 이 형식 지정 시 Location Store·factory 실행 전 `InvalidOperation`으로 거부; prefix·`entry` marker는 진단 정보(stable host identity·shard·application domain identifier 아님) | §7.3 | |
| R111 | Kubernetes에서 Pod IP·Pod별 DNS 이름을 AdvertiseHost로 사용 가능; RID·server identity·weight·수락·drain을 구분해야 하는 listener는 여러 Pod를 하나의 일반 Service 가상 주소로 대신하지 않음(각 Pod endpoint를 별도 발견·연결 가능해야 함) | §8 | |

### 5.5 `29-transport-liveness` + `49-internal-liveness-and-state` §1

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R112 | 세 연결 방식(RouteMesh·ClientServer·Classic fanout)에 같은 시간 기준 적용하나 확인 방법이 다름(양방향은 확인 요청·응답, fanout은 단방향 beacon) | 29 §1 | |
| R113 | RouteMesh 양쪽 Object role 모두 Client이면 peer connection 미사용(automatic은 descriptor 확인해 intent 미생성, manual은 handshake 확인 후 ready 전 close) → 이 pair에는 probe·deadline 미적용 | 29 §1 | |
| R114 | 확인용 command·raw transport monitor·timer는 application public API 아님, message handler는 이 신호를 미수신 | 29 §1 | |
| R115 | liveness는 Store owner 사용 기한, STREAM session heartbeat, request timeout과 각각 목적이 다름(하나가 다른 것의 대체 신호로 안 쓰임); Shutdown도 liveness 실패와 별개 operation | 29 §1 | |
| R116 | 고정값: 연결 확인 주기 5초(모든 RouteMesh·ClientServer·fanout connection), peer deadline 15초(정상 확인 없이 유지 가능한 시간); Framework builder는 이 두 값을 미공개, channel·handler·peer별 다르게 지정 불가 | 29 §2 | |
| R117 | 각 언어 service runtime은 public raw socket API+Framework service protocol만 사용(private binding member·native symbol 직접 호출·언어별 숨은 옵션 미사용) | 29 §2 | |
| R118 | ready = transport 연결+service handshake+identity 확인 통과, message target 사용 가능 상태; RouteMesh·ClientServer는 ready 시점부터 15초 deadline 적용 | 29 §3 | |
| R119 | Manual RouteMesh 양쪽 Object Client+Server membership 없는 pair는 `NotRequired` terminal(liveness failure·reconnect 대기 아님); 같은 endpoint·configuration generation 재시도 안 함; monitoring은 `not_required`를 `not_connected`와 구분, probe·deadline·liveness/health failure 집계에서 제외 | 29 §3 | |
| R120 | 확인 절차 5단계(0 아닌 새 ID 생성 → `livenessProbe`로 전송 → 응답 대기 중 다음 주기 오면 같은 ID 재전송 → peer는 `livenessAck`로 그대로 반환 → 대기 ID와 같은 첫 ACK만 deadline 15초로 재설정); connection마다 미응답 ID는 최대 1개 | 29 §3 | |
| R121 | 입력별 영향 표 5행(일치하는 첫 ACK→ID 제거+deadline 재설정 / 중복 ACK→불변 / 이전 probe ID ACK→불변 / 다른 physical connection ACK→불변 / 일반 message→마지막 수신 시각만 갱신, deadline 미연장); 15초 안에 올바른 ACK 못 받으면 not-ready로 바꾸고 닫음 | 29 §3 | |
| R122 | Probe·ACK는 업무 payload·metadata 미포함, application queue·handler 미실행 | 29 §3 | |
| R123 | PUB(송신 전용)·SUB(수신 전용)이라 subscriber가 같은 physical connection으로 ACK 불가 → `livenessProbe`·`livenessAck` 미사용; automatic은 publisher descriptor마다, manual은 endpoint마다 SUB socket+receive loop 1개; 여러 publisher를 한 SUB socket에 함께 연결 안 함(한 publisher timeout이 다른 publisher를 not-ready로 안 바꾸기 위해) | 29 §4 | |
| R124 | Publisher는 5초마다 같은 PUB endpoint로 liveness beacon 전송(app event 여부 무관); topic frame `01 5A 4C 46 31`, payload frame `5A 46 01 01`, frame 수 정확히 2개; application은 이 값과 정확히 같은 topic 미사용(지정 시 호출 인자 오류), 길이·byte 다르면 사용 가능 | 29 §4 | |
| R125 | Subscriber는 publisher별 socket에서 유효 application record 또는 정확한 beacon을 처음 받으면 ready; 이후 둘 중 하나 받을 때마다 마지막 수신 시각 갱신; 15초간 무수신 시 해당 publisher만 not-ready+전용 socket 닫고 현재 설정에 따라 재연결 | 29 §4 | |
| R126 | Beacon은 application record와 같은 PUB socket이라 Classic fanout 손실 규칙 적용(수신 queue 가득 참 시 beacon 버려지고 재전송 안 됨); host가 15초 넘게 포화 유지+fanout traffic이 계속 queue 채우면 해당 publisher not-ready(오탐 아님, 실제로 그 시간 동안 application record 미처리 상태) | 29 §4 | |
| R127 | 한 peer의 수신 독점은 오탐 — Framework는 한 connection의 연속 수신량에 상한(이 상한은 fanout 전용이 아니라 RouteMesh·ClientServer·service connection·STREAM 등 수신 단계를 공유하는 모든 경로에 적용); 상한 도달 시 남은 수신을 다음 기회로, 다음 회전은 이번에 멈춘 connection 다음부터 시작(항상 처음부터 순회하면 뒤쪽이 계속 밀림) | 29 §4 | |
| R128 | 상한은 건수·byte·경과 시간 3축을 함께 두고 먼저 닿는 것 적용(한 축만 두면 다른 축으로 독점 가능); 소켓이 여러 peer를 대표하면 socket이 아니라 peer 단위로 회계; **세 상한 값은 아직 미정** — 판정 가능한 것은 "무한정 읽지 않는다"와 "회전 시작점이 이동한다"까지 | 29 §4 | |
| R129 | Beacon은 application event 아님(응답 안 보냄, application queue·fanout handler 미전달, message trace 미생성, 수신 metric 미증가); topic이 예약값인데 payload 다르거나 frame 수 2개 아니면 protocol error, 해당 publisher만 즉시 not-ready+해당 socket만 닫음 | 29 §4 | |
| R130 | Ready 조건 표 2행(RouteMesh·ClientServer: transport+handshake+identity·generation 검증+handler 준비 모두 완료, Server membership 없는 Object Client pair는 ready 대상 제외 / Classic fanout: publisher별 SUB socket 연결+descriptor·manual endpoint 관계 유효+첫 정상 record·beacon 수신) | 29 §5 | |
| R131 | Ready target 즉시 제거 조건 7가지(orderly close, transport 오류·disconnect event, peer deadline 초과, fanout publisher 15초 무전송, identity·lifecycle generation·security 확인 실패, 같은 lifecycle generation 새 connection 승인으로 기존 physical connection 교체, host가 `Draining`·`Stopped`·`Error`) | 29 §5 | |
| R132 | Connection replacement는 현재 descriptor의 node RID·security identity·lifecycle generation을 admission fence로 사용; 완전한 descriptor 기대값 있으면 generation `0` endpoint-only manual intent가 검사 완화 못 함; `transportPairId`·`transportPairGeneration`으로 기존 pair 종료 요청 후 close snapshot·disconnect event 관찰했을 때만 교체; pair identity 없는 초기 연결은 endpoint-level disconnect 사용 가능하나 호출 성공만으로 physical close 완료로 판정 안 함, close 관찰 뒤에 새 connection 생성 | 29 §5 | |
| R133 | Orderly close·transport disconnect는 15초 안 기다리고 즉시 반영; 이전 physical connection의 늦은 ACK·frame은 새 connection 상태 불변 | 29 §5 | |
| R134 | Peer 하나의 실패가 host 전체를 `Error`로 안 바꿈(다른 ready peer·local owner는 계속 처리); ready peer 없으면 Channel 호출은 `NotFound` 또는 `Unavailable`; Framework는 timeout을 늘려 실패를 숨기지 않음 | 29 §5 | |
| R135 | Connection 잃은 시점별 처리 표(transport 수락 전→route-not-connected / 수락 여부 불명→다른 peer 자동 재제출 안 함 / 이미 수락→reply·timeout·cancellation·Shutdown·route failure 중 하나로 한 번만 완료); connection loss 뒤 request·one-way를 다른 peer·owner에 자동 제출 안 함 | 29 §6 | |
| R136 | Reconnect: RouteMesh·ClientServer는 handshake·identity 확인 재수행; NotRequired admission pair는 같은 manual configuration generation에서 재연결 안 함; 이전 connection ID·reply route·session binding·ready 상태 재사용 안 함; Classic fanout은 publisher용 SUB socket 새로 생성, 첫 정상 record 전까지 ready 아님; 같은 RID라도 현재 discovery descriptor와 다른 lifecycle generation이면 새 process 실행으로 처리(값의 숫자 크기는 비교 안 함) | 29 §6 | |
| R137 | Owner lease·descriptor는 discovery·배치 근거이나 transport ready 증명 아님; Store polling 실패해도 이미 연결된 peer의 transport 상태 확인 계속; probe·ACK·beacon 수신해도 만료된 owner lease·object owner를 다시 유효하게 안 만듦; owner lease 갱신 주기와 transport 확인 주기는 같은 값 아니며 같은 public option으로 안 합침 | 29 §7 | |
| R138 | `Relocate`·`Shutdown`이 새 작업을 막은 뒤에도 이미 수락한 reply·relocation·STREAM 처리에 필요한 connection은 deadline까지 유지 가능(새 target 선택에는 미포함); 종료 시 connection 닫기 전 liveness timer·reconnect timer·transport monitor subscription·pending callback을 끝냄 | 29 §7 | |
| R139 | Runtime snapshot 상태 6종(Configured intent, Connecting, Admitted, Ready, Reconnecting, Last failure); orderly disconnect와 peer deadline 초과는 다른 reason으로 기록; metric label에 endpoint·RID·connection ID 미포함(개별 identity는 항목 수 제한된 snapshot·trace에서만) | 29 §8 | |
| R140 | Application Job Queue pressure가 `paused`여도 그 사실만으로 route ready·host ready·transport liveness 불변(기존 topology별 progress 증거·deadline이 판정); receive-flow 적용 범위는 RouteMesh·ClientServer paired DEALER/ROUTER socket뿐, PUB/SUB·STREAM 제외; 새 대상 socket은 현재 pressure 절대 상태 적용 후 route 게시, `running`·`paused` 모두 같은 순서 | 29 §8(관측 소절) | |
| R141 | **결정** — mesh peer liveness 판단은 runtime 전체에서 하나의 구조가 소유; subsystem마다 다른 주기를 쓰면 같은 peer를 available/unavailable로 동시에 다르게 판단하는 구간이 생김; 설정으로 노출하지 않음(조정 가능하게 만드는 것 자체가 계약 위반) | 49 §1 | |
| R142 | **결정** — 업무 message 수신을 생존 신호로 안 씀(방향 비대칭 — peer에서 계속 받아도 이 node가 보낸 message가 도착하는지는 알 수 없음); 확인 신호·응답은 application에 미도달(handler 미실행, 일반 관측에도 미포함) | 49 §1 | |
| R143 | **결정** — 기준값은 같아도 방법은 topology마다 다를 수 있음(양방향은 확인 요청·응답, fanout은 발신자 주기 신호); STREAM session 연결 유지 신호는 목적이 다른 별도 신호, mesh peer 생존 판단을 대신 안 함 | 49 §1 | |
| R144 | **결정** — liveness 판정은 authority를 안 바꿈: liveness subsystem은 peer·owner lease availability evidence만 게시; location resolver가 evidence+authority를 함께 읽어 조회 결과 생성; lifecycle component만 explicit `Close`·`IdleEvicted` cleanup·정식 lifecycle operation으로 authority release; activation coordinator는 liveness event를 직접 안 받고 resolver의 `Missing` 결과만 받음(연결 장애 감지가 객체 생성·relocation·owner takeover 정책으로 누출 안 되게) | 49 §1 | |

**49 §2~§5는 이 대장에 포함하지 않는다** — §4 S9에서 정리했듯 liveness가 아니라 시작 순서·상태
공개·구독 backpressure·계측 비용이며, 이 주제(channel-transport)가 아니라 03-spot-actor(13)·
06-observability(24, 26) 주제가 그 규칙을 대장에 올려야 한다. 이번 재작성에서는 49 파일을
그대로 두므로 이 규칙들의 "옛 위치"만 여기 기록해 두고 "새 위치"는 해당 주제 매핑표에서 채운다.

| # | 규칙 요지 | 옛 위치 | 소유 후보 문서(재검토 대상) |
|---|---|---|---|
| R145(비고) | 상태 값 7종(`preparing`·`serving`·`relocating`·`relocated`·`draining`·`stopped`·`error`)의 닫힌 집합, 준비 완료는 `serving`일 때만 참 | 49 §3 | 24-runtime-monitoring §2.1 |
| R146(비고) | 시작 순서 5단계 — 등록 검증 → endpoint bind·실제 주소 확정 → 위치 저장소에 게시 → peer 수락·handler·객체 runtime 준비 → `serving`+신규 대상 선택 공개(3번이 5번보다 먼저여야 함) | 49 §3 | 13-mesh-node §6 |
| R147(비고) | 준비된 대상이 하나도 없어도 host는 시작하고 `serving`이 되며, 준비 안 된 channel은 그 topology만 저하 상태로 표시, 호출은 개별 실패 | 49 §2 | 24-runtime-monitoring §2.2 |
| R148(비고) | 상태 구독자·metric 수집기는 실행 권한 비점유, 자리 가득 차면 중간 상태 합치기(순번·밀림 횟수 증가분·최종 알림·버림 횟수는 보존), 구독 종료는 application 취소 시에만 | 49 §4 | 24-runtime-monitoring "합치기" |
| R149(비고) | 계측(message flow tracing) `off` 상태에서는 값 생성·문자열 조립·객체 할당 없이 읽기+분기로 끝남; 수준 변경은 그 뒤 message부터 적용 | 49 §5 | 26-message-flow-tracing §4.1 |

### 5.6 `51-internal-service-wire-protocol`

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R150 | `service-wire-v1.schema.json`이 유일한 규범 wire 정본(command ID, frame·layout, enum, field bound, durable format, semantic constraint 고정); 4언어 codec·상수는 이 schema에서 생성; W-2 전환 완료까지만 손작성 codec 유지, 새·변경 wire surface는 반드시 생성 경유, 새 손작성 encode/decode 경로 추가 금지; runtime은 source에서 layout을 갈라 정의·local compat encoding 추가·field 다르게 해석 금지 | §1 | |
| R151 | 계층별 규범 형식 4행 표(Location Store record=canonical JSON envelope, ClientServer application record=JSON `0xF2` envelope, internal mesh command·relocation stream=schema binary format, application payload bytes=opaque) | §1 | |
| R152 | Validator 명령(`validate-service-wire-schema.mjs --self-test`); wire major `1`, required capability `framework-service-v13`; schema/golden fixture 불일치나 undefined type·중복 ID·잘못된 enum·bound·conditional field 발견 시 build 중단 | §1 | |
| R153 | Location Store authority key 형식(Actor `zla1:a:<len>:<encoded-ActorId>`, Spot `zla1:s:<len>:<encoded-SpotRid>`); MeshName은 key 미포함(payload의 current placement attribute로만 저장); percent encoding은 RFC 3986 unreserved byte만 그대로, 나머지 uppercase hex | §1 | |
| R154 | Service record framing — Frame 0(prefix `Z`,`M`,wire major,command ID,flags + command body), Frame 1(metadata, flag `0x01`일 때만), 이어서 typed payload envelope(허용 시); multi-byte integer는 network byte order; 정의 안 된 flag·frame 수·conditional tail·trailing byte는 application dispatch 전 protocol error | §2 | |
| R155 | Decode 검증(allocation 전 complete length·item count·UTF-8 validity·모든 bound 확인); metadata frame ≤1,024 byte; application payload schema 절대 상한 = `applicationPayloadAbsoluteBytes` 4,294,966,774 byte; RouteMesh SS는 schema·wire 표현 절대 상한만, Framework-level message-size 상한 없음; ClientServer 실제 허용 크기 = schema 절대 상한과 `normalizedEffectiveMaxMessageBytes`에서 envelope overhead 뺀 값 중 작은 값; 숨은 16 MiB 고정 상한 없음 | §2 | |
| R156 | ClientServer complete-message 상한은 startup admission에서 결정; sender는 local·remote `normalizedEffectiveMaxMessageBytes` 중 작은 값, receiver는 자신의 admitted 상한 사용; admitted connection 수명 동안 불변, allocation 전 적용; 예시(양쪽 32 MiB → complete message 32 MiB 이내인 17 MiB payload 허용); RouteMesh admission에는 이 field 미포함, SS sender·receiver는 이 값으로 거절 안 함(HWM·mailbox byte budget·protocol 표현 한계는 별도 guard) | §2 | |
| R157 | Typed payload envelope는 packet name·contract 정보·serializer payload를 하나로 보존, raw frame 조합·codec table·maintenance field를 application code에 미노출 | §2 | |
| R158 | Framework multipart application profile — packet name `ZLinkFrameworkMultipart`, content type `application/x-zlink-multipart`; 4-byte BE part count → 각 part의 4-byte BE 길이 → 그 길이만큼 opaque bytes; part count ≥1; decoder는 count·length를 남은 범위와 대조, 모든 part 읽은 뒤 남은 bytes 있으면 거부; part 내용은 업무 의미로 미해석; Actor creation처럼 자체 envelope를 정의한 operation은 이 profile 대상 아님 | §2 | |
| R159 | 이 profile의 count·length·outer envelope·content-type frame·Framework metadata는 별도 Application byte HWM으로 재계산 안 함(Core byte charge는 complete message가 binding/Framework로 dequeue될 때 끝남, decode된 payload는 일반 소유권 규칙 따름); Framework는 receive/claim 전에 application job queue permit을 확보해 handler admission을 job 개수로 제한 | §2 | |
| R160 | 예약 command ID(`7..15`, `32`, `35`, `41`, `45`, `54..255`)는 다른 의미로 미재사용; 괄호 안 이전 이름은 호환 진단용일 뿐 decode·전송 대상 아님; 알 수 없는 command·반대 direction infra command·topology 미허용 command는 application queue 미투입 | §3 | |
| R161 | `messageFollow`(50)는 무응답 infra record, flags·app payload 미허용; body에 source/target route, hop count, relay 시점 queue count·byte(포화 방식 `u32` 진단 snapshot, `UINT32_MAX`는 "이상"이라는 뜻, admission 판단 미사용), 원래 operation ID·reply route ID; source·target route는 같은 object kind·identity 필요, 수신자가 source route의 target node가 현재 admitted peer인지 먼저 확인; hop count `1..8`만 허용; control envelope 최대 16 MiB(payload queue 상한 아님); 불일치 record는 dispatch 전 protocol error | §3.1 | |
| R162 | 통지 중복 억제 — source runtime은 현재 cache 항목이 source route와 동일한 exact route fence를 가리킬 때만 무효화(더 새 route면 안 지움); 통지 유실돼도 cache lifetime 지난 stale route는 반드시 만료; suppression registry는 source·target exact route fence 전체를 key로, 상태는 `idle → inFlight → sentUntilExpiry`(전송 실패 시만 `inFlight → idle`), route cache 만료·교체가 marker도 지움; registry는 payload·reply route·terminal completion을 미소유 | §3.1 | |
| R163 | `boundSessionReplaced`(51)는 새 Actor binding 확정 뒤 이전 session owner에 보내는 one-way infra record; Actor authority source fence+이전 owner의 exact lifecycle·binding identity 전달, flags·app payload·ACK 미사용; 보내는 node가 Actor authority target과 일치하는지 확인, 받는 node는 이전 session owner identity를 local target fence로 검사; 전송·이전 owner의 callback·연결 종료는 새 bind terminal을 지연·복원 안 함; 이전 owner는 exact retired identity만 적용; terminal(성공/실패) 뒤 `100 ms` 뒤 connection 닫음(outbound queue가 먼저 비어도 단축 안 함) | §3.2 | |
| R164 | Admission — RouteMesh·ClientServer는 `hello → admit|reject`로 승인; manual lifecycle token은 CSPRNG nonzero opaque equality token, 숫자 크기 비교 안 함, current physical connection의 handover·liveness로 이전 token 차단; store 기록 owner peer는 owner 자격·lease 유효성까지 확인 | §4 | |
| R165 | `DescriptorRevision`은 같은 lifecycle에서만 strictly increasing; 같은 revision·같은 bytes는 idempotent, 같은 revision 다른 bytes·낮은 revision은 protocol error; `update`가 바꿀 수 있는 값은 channel weight·runtime state·placement capacity·maintenance wave뿐; RID·topology·security identity·capability·application version·normalized message 상한은 재-admit 필요 | §4 | |
| R166 | Physical connection replacement는 descriptor admission과 같은 fence 사용; 완전한 descriptor 기대값 있으면 generation `0` endpoint-only manual intent가 값을 못 덮어씀; `transportPairId`·`transportPairGeneration`으로 현재 pair 모든 lane 종료 지정, close snapshot·disconnect event 받기 전 같은 endpoint 새 connection 미생성; pair identity 없는 초기 transport는 endpoint-level disconnect를 fallback으로 쓰되 호출 성공이 physical close 관찰을 대신 안 함; 늦은 이전 pair event는 pair identity로 fence되어 admission·ready 불변 | §4 | |
| R167 | ClientServer connection은 ChannelName 1개+client-to-server 방향 고정; infra command에만 service wire record 사용(client가 `hello`를 Core request로 시작, liveness 쌍 교환; server는 그 hello reply leg로만 `admit`/`reject`, `update`·liveness는 push); ClientServer application record는 service wire command 미사용 — `[JSON header(formatMarker 0xF2, kind request/response/command/error), payload]` 2-frame channel envelope 사용(request는 Core request envelope, response/error는 그 reply leg, one-way는 plain send); `channelSend`(18)/`channelRequest`(19)/`reply`(20)는 RouteMesh connection 전용; RouteMesh record를 ClientServer에 재사용(또는 반대)하면 protocol error | §4 | |
| R168 | Service liveness wire — 연결마다 outstanding ID 1개, 있으면 같은 ID 재전송; 이전 ID·중복 ACK·다른 connection ACK·다른 inbound traffic은 diagnostic만, deadline 미연장; orderly disconnect·raw transport failure는 즉시 not-ready(15초 미대기); probe·ACK·timer는 infra reserve 처리, application queue·handler 미전달; **admitted된 양쪽 peer가 모두 probe해야 함**(양방향 의무, dial 주체 무관, admission 순간 시작; ACK만 하고 자기 probe를 안 보내는 node는 비준수); probe·ACK는 admitted 물리 connection의 안정적인 현재 epoch로 전송(`scope: admitted-physical-connection-lifetime`), 이미 live인 connection에서 중복 re-dial·반복 hello/admit은 idempotent(supersede·generation 회전 안 함); superseded되었거나 미전달 generation으로 stamp된 probe·ACK는 결함, 상대는 "다른 connection ACK"로 조용히 버리고 양쪽 deadline 불변; 새 generation은 실제 새 물리 connection이 admitted connection을 대체할 때만 발급(변경 없는 descriptor의 매 inbound admission마다 발급 안 함) | §5 | |
| R169 | Classic fanout beacon wire — topic `01 5A 4C 46 31`, payload `5A 46 01 01`(29·08과 동일 값의 wire 출처); subscriber는 publisher마다 전용 SUB socket, 첫 유효 record·beacon에서 ready, 마지막 valid receive 15초 뒤 해당 publisher만 not-ready; reserved topic의 frame 수·payload 부정확 시 즉시 protocol error; 공개 topic을 유도한 결과가 예약 topic과 일치하면 transport 전 application 인자·configuration 오류로 거부 | §5 | |
| R170 | Typed application message JSON — `framework-json-v1` profile의 공개 encoding·validation 규칙은 Message model §2.3이 단독 소유, 이 문서는 별도 규칙을 정의하지 않고 parser 선택·buffer 재사용·원본 UTF-8 bytes 전달 방식만 내부로 다룸; relocation adapter application state는 이 profile 적용 대상 아님(opaque bytes로 저장, JSON parsing·state contract ID·app-specific version 비교 안 함) | §6 | |
| R171 | Store-backed authority는 provider 발급 `StoreVersion`·`ObjectGeneration`·`AuthorityOwnerGeneration`과 current host의 `OwnerId`·`OwnerLeaseGeneration`을 분리 보존; ObjectGeneration은 delete 뒤 같은 key로 새 object 만들 때만 변경; AuthorityOwnerGeneration은 owner 바뀔 때마다 증가해 낡은 owner 변경 차단; host owner lease token은 process 전체 공유 | §7 | |
| R172 | Creation record(Actor·User Spot manager create, target-owned Instance activation)는 generic reservation으로 final object·owner generation과 `Creating` row 생성; record는 object kind·global key·stable type·target descriptor·capacity delta·provider fence·최대 1 MiB complete request envelope의 content reference·hash 보존("stored creation intent"); Factory·initialize·initial membership 완료 후 같은 fence로 reservation commit+`Ready` CAS; target-owned Instance cold activation만 commit 전 durable activation inbox first record 추가 확정; Manager `Find`·ID-only messaging은 `Ready`만 사용; Entry Spot은 startup 뒤 host `Serving` 전에 publish, caller가 미생성 | §7 | |
| R173 | Factory 실패는 local barrier를 failed로 seal, waiting request 한 번만 terminal 처리; one-way operation은 drop event 기록; Store version·object·owner generation·owner lease가 모두 그대로일 때만 row 삭제, ambiguous 결과는 read로 reconcile; local registry는 `Missing` 확인까지 failed 상태 유지, 그 다음 caller만 새 `NewObject` 시작 가능; Object `Client`·`Server` role은 Location Store 필요, `None`은 authority·hidden local runtime 미생성 | §7 | |
| R174 | Cold activation recovery(§8)는 최초 cold activation이 Ready를 publish했지만 첫 operation의 terminal completion+recovery pointer 제거를 못 끝낸 경우에만, 같은 target node·lifecycle generation에서 그 operation 재개(일반 owner-loss reactivation 아님, steady Ready owner 종료·lease 만료에는 미사용, 다른 node 선택·factory 재실행 안 함) | §8 | |
| R175 | Missing+Instance intent envelope(`instance-activation-recovery-v1`)는 command 39의 optional metadata presence·frame까지 보존한 complete envelope를 Relocation Store에 저장, receipt를 Reserve에 연결; target-owned Instance cold activation 전용(Actor·User Spot generic create 미사용); command 39 route는 첫 byte+`u16` body length의 closed union — kind `1`(기존 Ready authority로 전달, object·owner·lease generation+StoreVersion), kind `2`(Missing cold activation 전용, target Mesh·node RID·lifecycle·Spot RID·stable type·descriptor version·deadline, authority fence 금지); kind 2 route와 ZLIA의 target Mesh·stable type·descriptor version·deadline·operation identity·metadata presence·bytes 불일치 시 reservation 전 protocol error | §8 | |
| R176 | Target host는 startup 첫 scan+속도 제한된 background scan에서 자신이 소유한 Pending·미완료 최초 operation 가리키는 Ready Instance activation recovery root 재개(authority의 target node RID·lifecycle generation이 현재 host와 정확히 일치해야 함); scan·late control record는 object key·object·owner generation·owner lease로 정한 local barrier 하나로 수렴; Ready 전 durable inbox first record 확정, handler는 barrier로 막고 startup은 queue head 복원 전 Serving 미게시; 첫 handler terminal completion을 durable 기록+replay cursor를 inbox sequence까지 갱신한 뒤에만 recovery pointer를 Preserve CAS로 제거(queue admission만으로는 제거 안 함) | §8 | |
| R177 | Cold activation recovery 실패는 local barrier seal+request 한 번만 terminal+one-way drop event 기록; fence 값 일치할 때만 삭제 후 재읽기로 맞춤; delete 전 process 종료 시 target scan이 retry-safe factory 재실행 가능; `Missing` 확인 전에는 새 activation 미시작 | §8 | |
| R178 | Command 47(User Spot remote create) — source는 generic Reserve 이후 correlation·operation ID·source node lifecycle·global Spot RID·stable type·provider 발급 reservation fence·deadline을 대상 하나에 전송; fence는 expected StoreVersion·object·owner generation·target node lifecycle·owner lease·pending capacity 보존; target은 Pending creation projection의 immutable content를 Store에서 읽음(command 47에 application payload·metadata 없음). Command 48(User Spot remote close) — source node lifecycle·operation identity 외 정확한 `SpotRef`·target node lifecycle·AuthorityOwnerGeneration·StoreVersion 전송; target은 current authority·Actor membership·relocation state를 수용 판단 전 검사; 두 command 모두 RouteMesh infra command, flags·payload 미허용 | §8.1 | |
| R179 | 두 operation 결과는 command 20 reply envelope; create 성공 tail = `Existing`·`Created`·`Rejected` discriminator+`SpotRef`(app reply는 `Existing`에서 금지, `Created`·`Rejected`에서만 선택적 허용); close 성공 tail = `closed` bool; source operation table이 (source RID·lifecycle, operation ID)로 terminal-once 보장; Location row polling·application packet 제어 message는 reply를 대신 안 함 | §8.1 | |
| R180 | `actorJoin`(28)은 `[request]`로 전송, 결과는 command 20 reply(그 request의 `[reply]` leg)로만 반환; body(correlation, actor route fence, `entry` flag, target spot route fence)가 완전한 cross-language 계약, 그 외 wire field 없음; 내부 transfer bookkeeping identifier는 language-internal, 이 body에 미노출(필요한 runtime은 local 생성·유지, wire 미포함); one-way로 송수신하면 계약 위반 → dispatch 전 protocol error; 기존 준비 대기·later-attempt-wins 규칙은 이 body의 actor identity를 key로 사용(내부 id 아님) | §9 | |
| R181 | Join 수락 reply의 app reply는 Framework multipart application profile로 감싸 reply leg에 실음(part 정확히 1개, handler 반환 bytes 그대로); app reply 없으면 multipart envelope 자체 미전송; source는 profile을 언랩해 sole part를 app reply로 전달, 다시 다른 envelope로 해석 안 함; 노출 reply metadata는 profile 고정 outer content type이거나 없음(request의 content type 등으로 재구성 안 함); 이 profile 외 프레이밍(중첩 envelope, 언랩 안 한 inner bytes 노출)은 계약 밖 | §9 | |
| R182 | 수신자 stable-type 해석 — `actorJoin`(28) body는 Actor stable type을 의도적으로 미포함, 수신자는 canonical Location Store Actor Authority row(`authority\0actor\0{ActorId}`)에서 해석; admission은 body의 `ActorId` row를 **반드시 읽고** actor route fence와 5조건(state==active&objectKind==actor, objectGeneration 일치, owner node RID·lifecycle generation 일치, authorityOwnerGeneration 일치, ownerLeaseGeneration 일치) 모두 일치할 때만 수용, 그 뒤 `allocation.stableType`으로 local factory 해석(relocationState(52)의 Authority 유도 stable-type 검증과 동일 패턴); 모든 실패는 command 20 reply의 typed terminal(조용한 drop 아님) — 없거나 읽을 수 없는 row는 `Unavailable`/`NotFound`, 불일치 fence field는 stale/mismatch protocol terminal, 미지 stableType은 typed rejection; generation은 exact equality만(§12); source의 `expectedOwnerLeaseGeneration`은 Actor의 현재 Location owner lease(bound-Session token 아님) — unbound Actor도 자기 owner lease 실음, 수신자는 canonical 수용에 bound Session을 요구하면 안 됨 | §9 | |
| R183 | Session seal & source relay — relocation coordinator는 source dispatch를 멈추기 전 command 42로 bound Session binding seal, command 43은 설치 결과만 반환; Session owner는 current Session identity·binding generation·ActorId·ObjectGeneration·relocation identity만 확인(numeric high-water·Actor authority 재조회 없음); seal 뒤 Session request·push는 route 변경·abort까지 Session owner가 보관(record 수·byte 상한 없음); source object route로 들어온 일반 server message는 target temporary queue로 계속 relay(같은 TCP connection 순서·재전송 사용, message별 ACK·durable journal 미추가) | §9 | |
| R184 | Relocation manifest·direct chunk transfer — command 40 `relocationPrepare`(`[request]`)는 object identity·source node RID·generation·`payloadTotalLength`·`payloadChunkCount`·`payloadChecksumCrc32c`(relocation-root pointer·Store lookup key 미포함, Prepare 하나로 direct transfer 완전히 설명); target은 temp queue 준비됐을 때만 command 30 `relocationReady`(`[reply]`) 반환, 선언한 manifest 외 협상 없음; command 52 `relocationState`(`[send]`) 1개 이상은 relocation·targetAttemptGeneration·coordinator fence, object identity, `senderRole`, 0-base `chunkOrdinal` 포함(chunk bytes는 기존 `relocation-data-chunk-v1` 재사용), target은 즉시 assembly buffer로 복사 후 Core receive lease 즉시 반환(backlog queue 보관·lease 이전 없음) | §9 | |
| R185 | Target은 조립 길이·CRC-32C를 Prepare 선언값과 비교, 불일치는 명시적 실패(부분 조립 복원·투명 재시도 없음) — 실패 시 부분 chunk·준비 자원 정리 후 대응 Prepare에 command 53 `relocationFailed` reply(이 명시적 실패 수신만이 source의 capture payload 복원·operation 실패 조건, 연결 단절 같은 불확정 결과는 source 관점에서 비가역); command 31 `relocationData`는 capture 뒤 ingress-hold application record만 운반(saved queue 작업·timer는 오직 command 52로만 이동, record별 ACK·numeric high-water 없음) | §9 | |
| R186 | Source는 ingress-hold relay prefix 뒤 command 34 `relocationCutover`(`[send]`)에 `boundaryRecordCount`·`boundaryChecksumCrc32c` 포함(target 응답 없음, reserved ID 32·35·41 미송수신); cutover 미도달 확인+source instance 생존 시 새 connection으로 pending batch 전체+새 cutover 재전송(꼬리만 아님), target은 부분 staging batch를 이어붙이지 않고 통째 교체; 재전송 창 = `RelocationCutoverWaitTimeout`(기본 1,000 ms, 설정 가능), 지나면 CAS fallback 적용(Warning+counter만, 추가 blind retry 없음) | §9 | |
| R187 | Source는 application state·relocation 시작 전 미실행 queue·timer 정보를 direct transfer용으로 저장(native timer handle·callback continuation 미encode); target은 temp queue 등록 뒤 factory·chunk 조립 실행, 마칠 때까지 application handler 미실행; source는 cutover 전 받은 message 모두 relay 후 같은 ordered connection에 cutover `[send]`(reply 미사용); 일반 server-to-server send에 relocation 전용 app ACK 미추가, request는 기존 operation identity·correlation·deadline·caller retry 유지 | §9 | |
| R188 | CRC-32C 규약(polynomial `0x1EDC6F41`, initial `0xFFFFFFFF`, reflected in/out, XOR out `0xFFFFFFFF`, `check("123456789")==0xE3069283`); wire major `1`/required capability `framework-service-v13`(command 52·53+manifest field로 v12에서 상향, 4언어 동시 승급); target 유효 수신 chunk-byte 상한은 admission-accept reply의 `receiveChunkLimitBytes`로 협상하거나 host preflight로 협상, 협상 경로 없는 `JoinEntrySpot`은 32 KiB 유효 상한; 형태·상한은 `actor-join-reply-v1.json` golden fixture가 고정, 4언어(cpp·dotnet·java·node) 동일 decode; capability 확인되면 4언어 모두 canonical `actorJoin`(28)을 `[request]`로 originate하고 reply에 `receiveChunkLimitBytes` 포함, 미확인 시 언어-내부 admission 경로 유지(과도기 폴백); 수신측은 stable type을 wire가 아니라 §9대로 Store Actor Authority row에서 해석 | §9 | |
| R189 | Target CAS — chunk 조립+temp queue 등록 후 cutover 수신 시 Location Store owner·membership을 source→target CAS(target만 실행); Restore 준비 reply 뒤 `RelocationCutoverWaitTimeout`(기본 1,000 ms) 안에 cutover 미도달해도 `cutover_timeout` Warning 기록 후 같은 CAS 시작; source·Session owner는 timeout·local mirror·Session route 결과로 Location Store 변경 안 함; Relocation Store는 더 이상 Actor·Spot relocation payload 미보관(direct chunk transfer가 유일 handoff, 두 Store 모두 distributed transaction·2PC 미사용); Relocation Store 남은 역할은 Instance Spot cold activation envelope(§8)+relocation 후 pending request terminal record뿐 | §9 | |
| R190 | CAS 실패 시 target queue 미개방, Restore operation의 절대 deadline까지 같은 CAS retry(Relocation Store 보존과 무관); 불확정 응답이면 Store 재읽기로 exact target owner 먼저 확인, 다른 valid owner·generation 확인 시 stale relocation으로 즉시 종료; deadline까지 target owner 미확인 시 `location_update_failed` Error 기록, target 준비 Actor·Spot·temp queue·relocation state 제거(Session route는 미변경), 늦은 Store 응답이 종료된 `RelocationId` 재활성화 안 함; CAS 성공 시 source로 rollback 안 함 | §9 | |
| R191 | `RelocationId`는 runtime 생성 nonzero 128-bit 값(중복 control message 구분용, application 미노출); CAS 전에는 source가 owner, target은 Restore 끝내고 cutover·1,000 ms fallback을 기다리는 준비된 instance일 뿐 application message 미실행; target-only CAS 성공 시점부터 target이 owner, 같은 `ObjectGeneration` 유지하며 owner generation만 증가; Actor 하나·`PerActor` Spot authority·`SpotWide` aggregate·Instance Spot 모두 같은 원칙(여러 owner·membership을 target이 조건부 batch 한 번으로 모두 바꾸거나 아무것도 안 바꿈); relocation은 participant 수·relay record 수·byte 수에 별도 runtime capacity gate 미추가(Store provider·transport 기존 frame·page 제한만 적용) | §10 | |
| R192 | Commit 뒤 queue·Ready 5단계(저장된 기존 작업·timer를 target execution queue에 → cutover 앞까지 relay된 작업을 그 뒤에 → temp queue 추가 작업+dispatch 경로 전환 → lifecycle callback 마치고 application dispatch 열기 → command 44 route update를 Session owner에 `[send]`); 서로 다른 TCP connection 사이 message의 전역 순서 미보장(target queue가 수락한 순서만 유지); owner 전환 뒤 이전 주소 도착 message는 Message Follow가 target에 전달 | §10 | |
| R193 | Source는 cutover `[send]` submit이 성공·실패 terminal에 도달한 뒤 target 완료 응답을 안 기다림; relay-ready reply가 accepted 상태 되기 전 명시적 target 실패만 abort하여 source queue·Session seal 복원, 그 뒤 submit 실패는 source 복원 안 함; cutover가 늦거나 중복되면 target은 `late_cutover` Warning만 기록, state 재변경 안 함; 1,000 ms fallback으로 queue 연 경우 늦은 relay가 새 target direct message보다 먼저 실행된다고 보장 안 함 | §10 | |
| R194 | Session route는 Session owner의 current Session·binding에서만 검증; command 42는 current binding seal, command 43은 exact seal 설치 결과만 반환(message sequence·high-water 미전달); command 44는 route update면 target runtime이, accepted 전 abort면 source coordinator가 `[send]`로 전달(relocation identity·current binding generation·ActorId·ObjectGeneration·target route 포함, Session owner는 Location Store·Actor authority mirror 재조회 안 함); Session owner는 route·current `ActorRef` snapshot을 target으로 바꾸고 seal 중 보관 message를 target route로 제출 후 seal 해제; command 44에는 reply 없고 reserved command 45 미송수신 | §10 | |
| R195 | `SessionRelocationSealTimeout` 기본값 3,000 ms; exact command 44가 그 안에 안 오면 Session owner는 physical Session을 닫고 binding·held message·seal state 정리; timeout 뒤 늦은 command 44·exact duplicate는 Warning만, route·seal·authority 재변경 안 함; target이 accepted 전 명시적으로 실패하면 matching seal만 해제하고 보관 Session message를 source route로 제출, 그 뒤 failure·cutover submit 실패는 source route를 다시 안 엶 | §10 | |
| R196 | Transport adapter의 authenticated peer·node generation·frame 검증, target의 owner CAS, Session owner의 binding route 검증은 각각 정확히 한 번만 수행(Actor join·host relocation·Message Follow·callback 경로는 재수행 안 함) | §10 | |
| R197 | `OperationId`는 두 `u64` word(`high`,`low`)의 non-zero identity, `ReplyRouteId`는 별도 non-zero `u64`; 둘 다 source owner lifecycle 안에서 unique, wrap·reuse는 terminal runtime error; OperationId는 dedup identity이며 reply route를 대신 안 함(registry·durable record는 한 word로 안 줄임); durable terminal identity = 불변 `RelocationId`+시작 쪽 fence+`OperationId` 조합 | §11 | |
| R198 | Target은 terminal completion·delivery state를 새 immutable relocation root에 쓴 뒤 authority CAS로 `TerminalCompletionCount`·`PendingRelayCount`를 함께 갱신; `replyRelay`는 원래 reply route+source lease fence 사용; source는 terminal result 수락 또는 이미 terminal 확인 후 authenticated `replyRelayAck` 전송; physical connection close는 terminal delivery 증거 아님 | §11 | |
| R199 | `Completed`는 accepted request count == terminal completion count이고 pending relay가 0일 때만 허용; source lease 유효한 동안 ACK 미확인 시 Retire는 relocation root·reply bytes를 retention 동안 보존한 채 `ForceStopped`로 끝남 | §11 | |
| R200 | Root replacement는 새 immutable root의 reference·checksum·inventory digest 검증 후 authority CAS로 연결; conflict loser root는 orphan 정리; cleanup은 Location authority reference release 후 Relocation Store delete; published reference의 permanent missing·checksum mismatch·inventory digest mismatch는 non-retriable `RelocationDataLost`, commit된 owner·membership을 source로 rollback 안 함 | §11 | |
| R201 | Schema `SendReady` record kind `12`는 service control 전용; Core 0.13의 operation별 `send_completion`·binding awaitable은 HWM 재시도 완료를 전달하는 별도 계약; binding에서 send-ready callback이 폐기돼도 service-wire record·schema 값은 유지 | §11 | |
| R202 | Wire command 자체는 우회 권한 아님, ordinary control·malformed record도 shared permit 사용; 분류 전 permit은 46(dispatch loop), ordinary record storage 수명은 50(Payload 소유권)이 소유(cross-topic 링크만) | 말미 | |

(§12 구현 검증 12항목은 대장에 개별 R행으로 올리지 않고, 재작성 시 `wire-protocol` 문서의
검증 요구 절 형식(가이드 §9.3)에 맞춰 그대로 옮긴다 — 이미 인터페이스 관찰에 가까운 checklist라
표현만 정리하면 된다.)

## 6. 링크·코드·site 영향

| 대상 | 처리 |
|---|---|
| spec 내부 링크 23개(§1의 파일 목록) | 새 경로·새 절 anchor로 치환. 07·08·09·10·29는 절 제목이 바뀌므로 anchor 치환표를 §3.1~§3.5 매핑에서 생성. 51은 절 번호·제목을 유지하므로 경로만 바뀐다 |
| 코드 주석 2곳(§1) | `08-channel-messaging.ko.md` §3.2 인용을 `02-channel-messaging.ko.md` §3.2로 갱신(S6에서 절 번호를 그대로 유지하도록 재작성하므로 anchor는 안 바뀜, 경로만 바뀜) |
| cpp layout contract test | 영향 없음(이 7개 문서를 경로로 안 열음) |
| `target-readme.ko.md` 오타 | §3의 `05-transport-liveness`/`05-wire-protocol` 번호 중복을 `05`/`06`으로 정정 필요 — **이 매핑표는 산출물 파일 1개 외 수정 금지 범위라 여기서는 고치지 않고 코디네이터에게 남긴다** |
| `topic-map.ko.md` "29(+49)" 표기 | §4 S9대로 "29(+49 §1)"로 정정 필요 — 마찬가지로 코디네이터가 처리 |
| mkdocs nav | 캠페인 말미 site 작업에서 "Channel and transport" 그룹 → `02-channel-transport/README`, `01-channel-topology`, `02-channel-messaging`, `03-client-server-channel`, `04-network-listener-identity`, `05-transport-liveness`, `06-wire-protocol` |
| redirect | `07-channel-topology`→`02-channel-transport/01-channel-topology`, `08-…`→`02-…`, `09-…`→`03-…`, `10-…`→`04-…`, `29-…`→`05-transport-liveness`, `49-…`→`05-transport-liveness`(§1만, 나머지는 §4 S9의 재검토 결과에 따라 결정), `51-…`→`06-wire-protocol` |
| 검증 | `check_doc_links.py`, `mkdocs build --strict`, `git diff --check` |

## 7. 구현 대조 — 에이전트 입력

재작성이 끝나면 언어별 에이전트 4개(dotnet·jvm·cpp·node)에 다음을 준다.

- 입력: 새 6개 문서(`01-channel-topology` ~ `06-wire-protocol`)와 §5 대장(새 위치 열 채운 것).
  `wire-protocol`은 §12 구현 검증 checklist도 함께 준다
- 과제: 대장의 행마다 해당 언어 구현에서 **일치 / 불일치 / 스펙 미정 / 판단 불가** 중 하나와
  근거(파일:줄). R128(수신 상한 3축, 값 미정)은 처음부터 "스펙 미정"으로 보고하고 각 언어가
  실제로 쓰는 값(있다면)만 사실로 보고
- 금지: 스펙 수정, 코드 수정, 판정. 하위 에이전트를 띄우지 말고 직접 grep
- 출력 양식: `| R# | 판정 | 근거 | 비고 |`

판정(Claude): 옛 문서·4개 구현이 일치하는데 새 문서만 다르면 **재작성 오류** → 수정. 옛
문서 때부터 달랐거나 언어끼리 다르면 **spec gap** → [spec-gap 대장](../../spec-gap.ko.md) 등록.

## 8. 작업 순서

1. `02-channel-transport/README.ko.md` 초안(§2 질문표 기준)
2. `01-channel-topology`(07) 재작성 → §5.1 R1~R44 새 위치 채움
3. `02-channel-messaging`(08) 재작성 → §5.2 R45~R69 (07과 병렬 가능, 서로 다른 문서지만 §3의
   S2·S3·S5 중복 규칙을 어느 문서가 소유하는지는 프롬프트에 고정 문자열로 명시해야 함)
4. `03-client-server-channel`(09) 재작성 → §5.3 R70~R90 (07·08과 병렬 가능)
5. `04-network-listener-identity`(10) 재작성 → §5.4 R91~R111 (독립적, 병렬 가능)
6. `05-transport-liveness`(29+49§1) 재작성 → §5.5 R112~R144 (독립적, 병렬 가능. 49 §2~§5는
   손대지 않고 원문 그대로 남겨 둘 것을 프롬프트에 명시)
7. `06-wire-protocol`(51) 재작성 → §5.6 R150~R202 (절 번호·제목 유지, 문장 정리만; 다른 5개
   문서 완료를 기다릴 필요 없이 병렬 가능하나 분량이 크므로 별도 에이전트로 먼저 시작하는 편이
   유리)
8. 등가성 대조 — 대장 빈 행 0, 추가 보장 0
9. en 짝 작성(마지막 단계, §5 순서대로)
10. 링크 치환·nav·redirect·`target-readme.ko.md`·`topic-map.ko.md` 정정 → 검증 3종 그린
11. 구현 대조(§7) → 판정·기록
12. 한 커밋(문서 이동+내용) + spec-gap 대장 갱신

## spec-gap 후보

이번 주제를 읽으며 나온 것은 **구조 배치 문제**(§4 S8·S9·S10)이지 계약 결함이 아니므로 이
절에는 해당 사항이 없다. 굳이 기록해 둘 만한 것은 다음 하나뿐이며, 계약의 값 자체가 비어 있는
경우다.

- **G-candidate 1**: `29-transport-liveness.ko.md` §4(51 §5도 동일)의 수신 독점 방지 상한
  3축(건수·byte·경과 시간)의 실제 값이 스펙에 없다. "무한정 읽지 않는다"와 "회전 시작점이
  이동한다"만 판정 가능하다고 원문이 명시한다(R127~R128). 구현 대조 단계에서 4언어가 실제로
  쓰는 값을 비교해 값을 하나로 정할지, 언어별 재량으로 남길지 판단이 필요하다 — 새 값을 만들지
  않고 spec-gap 대장에 등록만 한다.
