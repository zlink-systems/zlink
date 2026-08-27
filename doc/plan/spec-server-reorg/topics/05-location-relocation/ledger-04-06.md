# 04-relocation-flow · 05-host-relocation-flow · 06-failure-failover-policy 대장

> 매핑표: [mapping.ko.md](mapping.ko.md) §5.4~§5.6(R177~R315). 새 위치는 절 번호로 적는다.
> "새 위치" 열의 절 번호는 각 새 문서의 `## N.` 제목 번호를 가리킨다. R# 각 행의 핵심 문구를
> 새 문서에서 grep으로 실물 확인했다(문구는 원문 표현 그대로가 아니라 재작성된 표현으로
> 존재 — 아래 각 문서 절에서 실제로 다루는 내용을 재확인했다).

## 04-relocation-flow (28 + 44 + 52 병합) — R177~R237

| R# | 새 위치 | 비고 |
|---|---|---|
| R177 | §1 | |
| R178 | §3 | |
| R179 | §3 | |
| R180 | §3 | |
| R181 | §3 | handoff 값 인벤토리 표 포함(52 §2 흡수) |
| R182 | §4.1 | |
| R183 | §4.2 | |
| R184 | §4.2 | |
| R185 | §4.2 | |
| R186 | §4.2 | |
| R187 | §4.2 | |
| R188 | §4.3 | |
| R189 | §4.3 | |
| R190 | §4.3 | |
| R191 | §4.3 | |
| R192 | §4.4 | |
| R193 | §4.4 | |
| R194 | §4.4 | |
| R195 | §4.4 | |
| R196 | §4.4 | |
| R197 | §4.4 | 1,000ms 값의 단일 출처. 05 §9는 이 값을 재정의하지 않고 인용만 한다 |
| R198 | §4.4 | |
| R199 | §4.4 | |
| R200 | §4.4 | |
| R201 | §4.5 | |
| R202 | §4.5 | |
| R203 | §4.5 | |
| R204 | §4.5 | |
| R205 | §4.5 | |
| R206 | §4.6 | |
| R207 | §4.6 | |
| R208 | §4.6 | |
| R209 | (§4.6 diagram) | source 실행 종료·Message Follow 유지는 다이어그램 마지막 alt 블록 |
| R210 | §2 표(Session owner 행 요약) + §7 | Session owner 상세 서술은 세션 문서로 전량 이관, §2·§7은 3~4문장 요약과 링크만 남김 |
| R211 | §5.1 | |
| R212 | §5.2 | |
| R213 | §5.3 | |
| R214 | §5.3 | |
| R215 | §5.3 | |
| R216 | §5.3 | |
| R217 | §5.3 | |
| R218 | §6 | |
| R219 | §7 | Session owner 검증값 4개는 세션 문서 §8.1이 소유, 04 §7은 "요청하는 것은 세 가지" 요약만 |
| R220 | §8 | |
| R221 | §9 | |
| R222 | §9 | |
| R223 | §9 | |
| R224 | §9 | |
| R225 | §4.4 + §9 | fallback이 TCP retransmission 대체가 아니라는 문장은 §4.4로, reconciliation과의 관계는 §9로 분리 배치 |
| R226 | §9 | |
| R227 | §10 | |
| R228 | §10 | 30초·8hop·순환=Unavailable·generation불일치=InvalidOperation 값은 [Location runtime](../../../../../framework/doc/framework/common/spec/server/05-location-relocation/01-location-runtime.ko.md)로 링크만(파일 링크, anchor는 병렬 작업 완료 후 추가 — 아래 "나중에 anchor를 붙일 링크" 참고) |
| R229 | §10 | |
| R230 | §10 | |
| R231 | §12 | |
| R232 | §11 | 마지막 3개(부분조립 자가복구 금지·귀속판정 금지·dual-prewarm 금지) net-new 포함 |
| R233 | §11 | |
| R234 | §11 | |
| R235 | §11 | 언어별 재량 문구(phase representation)와 확인 기준을 §11 첫 문단에 통합 |
| R236 | §13 | 28 §12 + 44 §6 + 52 §10 통합, 중복 항목 제거 |
| R237 | §10 | |

## 05-host-relocation-flow (30 재작성) — R238~R291

| R# | 새 위치 | 비고 |
|---|---|---|
| R238 | §1.1 | |
| R239 | §1.1 | |
| R240 | §2.1 | |
| R241 | §2.1 | |
| R242 | §2.2 | |
| R243 | §3 | |
| R244 | §3 | |
| R245 | §3 | |
| R246 | §3 | |
| R247 | §4 | |
| R248 | §4 | |
| R249 | §5 | |
| R250 | §5 | |
| R251 | §5.1 | |
| R252 | §5.1 | |
| R253 | §5.1 | |
| R254 | §6 | |
| R255 | §6 | |
| R256 | §7 | |
| R257 | §7 | |
| R258 | §7 | |
| R259 | §7 | |
| R260 | §7 | |
| R261 | §8 | |
| R262 | §8 | |
| R263 | §8 | 1,000ms(04 §4.4)와 1초(이 문서 §8)가 다른 개념임을 §8 본문에 명시 — spec-gap 후보가 아니라 구조 정리(mapping §"spec-gap 후보" 참고) |
| R264 | §8 | |
| R265 | §9 | §8.2 mechanics 전체를 04 §4 링크로 축소(mapping §4 S3) |
| R266 | §9 | |
| R267 | §9 | |
| R268 | §9 + §7 | temporary queue 무상한은 §9, SessionRelocationSealTimeout 3,000ms는 세션 문서 §8이 소유 — 05는 재정의하지 않고 §12에서 링크만 |
| R269 | §10.3 | |
| R270 | §10.3 | |
| R271 | §10.5 | |
| R272 | §11 | |
| R273 | §12 | |
| R274 | §12 | |
| R275 | §12 | |
| R276 | §12 | Session 행은 04 §7·세션 문서 §8 링크로 교체(mapping §4 S1·S14) |
| R277 | §12 | |
| R278 | §13 | |
| R279 | §13 | 마지막 행(Session route update 적용)은 04 §7·세션 문서 §8 링크로 교체 |
| R280 | §13 | |
| R281 | §13 | |
| R282 | §13 | |
| R283 | §13 | |
| R284 | §13 | |
| R285 | §14 | |
| R286 | §14 | |
| R287 | §14 | |
| R288 | §14 | |
| R289 | §14 | |
| R290 | §15 | |
| R291 | §16 | |

## 06-failure-failover-policy (31 재작성) — R292~R315

절 구성은 원문 그대로 유지(mapping §3.7). 새 위치는 원문과 동일한 절 번호다.

| R# | 새 위치 | 비고 |
|---|---|---|
| R292 | §1 | |
| R293 | §1 | |
| R294 | §2 | 4단계에 accepted 경계 정의 출처를 04 §4.4로 명시하는 문장 추가(mapping §4 S13) |
| R295 | §2 | |
| R296 | §3.1 | |
| R297 | §3.1 | |
| R298 | §3.2 | |
| R299 | §4.1 | |
| R300 | §4.2 | |
| R301 | §4.2 | |
| R302 | §4.2 | |
| R303 | §4.3 | |
| R304 | §4.4 | |
| R305 | §4.4 | |
| R306 | §4.4 | |
| R307 | §5 | |
| R308 | §5 | |
| R309 | §6 | |
| R310 | §6 | |
| R311 | §6 | |
| R312 | §7 | |
| R313 | §7 | |
| R314 | §8 | |
| R315 | §10 | "정본" → "이 문서가 공개 장애 동작의 소유 문서다"로 정정(mapping §4 S12) |

## 52 §5 abort 순서 정정

이 abort 경로에는 서로 다른 두 축의 순서가 있다 — (a) **coordinator 축**: source가 durable
abort와 source queue 복원을 먼저 확정한 뒤에만 command 44 abort를 one-way로 보내는가, (b)
**Session owner 내부 축**: 그 abort를 받은 Session owner가 matching seal을 먼저 해제하는가,
아니면 held message를 source route로 먼저 제출하는가.

(a)는 28(299-301행)·44(§4 121행 요약)·52(240-241행) 세 문서가 이미 같은 순서로 쓰고 있었다 —
이 축은 이번 병합에서 어긋난 적이 없다.

(b)는 원문 대조 결과가 spec-gap G7·judgment.ko.md J3의 서술과 다르다. **28 자신의 원문
(300-301행)도 52(243행)와 같은 "held message를 source route에 제출하고 matching seal만
해제"(제출→해제) 순서를 쓴다** — 이 지점만 보면 52가 "유일한 이탈"이 아니라 28도 같은
순서다. 반대로 [Session과 Actor binding
「8.1」](../../../../../framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md#81-seal-held-message와-route-전환)(460-461행)은
"matching seal을 해제하고 보관한 Session message를 source route로 다시 제출"(해제→제출)
순서를 쓴다. judgment.ko.md J3은 이 축을 4언어 구현과 대조했고, dotnet(held **폐기**,
J9로 별도 기록)을 제외한 jvm·cpp·node 세 언어 모두 **해제→제출**로 구현되어 있어 "52가
유일한 이탈(제출→해제). 새 문서(20·48 순서)가 맞음"으로 판정했다 — 이 판정은 28의 정확한
현재 문구까지는 대조하지 않았지만, 구현 3개가 일치하는 순서를 "맞음"으로 확정했으므로
28의 문구도 같은 이유로 정정 대상이다(28은 이 병합으로 대체되므로 별도 문서 수정은
필요 없다).

병합 문서 `04-relocation-flow.ko.md` §4.4는 (a) coordinator 축은 그대로 유지하고, (b) Session
owner 내부 축은 **session 문서 §8.1이 소유한다고 링크만 하고 이 문서에서 다시 정의하지
않는다** — 두 옛 문서(28·52)가 서로 다른 순서를 재서술하며 어긋난 지점이므로, 병합 문서가
같은 문장을 세 번째로 재서술하지 않는 편이 재발을 막는다. Abort branch sequence diagram의
Session owner local 단계도 "matching seal 해제와 held message의 source route 재제출"로
순서를 특정하지 않는 병렬 표현으로 바꿨다. §13 검증 요구는 "source queue 복원이 command 44
abort 전송보다 먼저 확정된다"는 coordinator 축만 명시하고, Session owner 내부 축의 검증은
session 문서 §14로 넘긴다. spec-gap 대장 G7의 "결정" 열은 이 문서 완료 시 "반영 완료"로
갱신하되, 비고에 "28도 52와 같은 순서였다 — judgment.ko.md J3의 '52가 유일한 이탈' 서술은
28 원문 재대조 없이 내려진 것"이라는 정정을 함께 남긴다.

## Session 소유로 넘긴 서술

아래 위치의 Session owner 상세 서술(seal 설치, held message 보관, route 전환, command
42·43·44 처리)을 `04-session/02-session-actor-binding.ko.md` §8 링크로 교체했다. mapping §4.1의
전체 위치 목록에 대응한다.

| 옛 위치 | 04/05에서의 처리 |
|---|---|
| 28 §2 표(38-40행) Session owner 책임 요약 | 04 §2 표에 2문장으로 축약, 상세는 링크 |
| 28 §4.1(88-90행) Seal 설치 시점 | 04 §4.1에 1문장 유지(handoff 순서상 필요한 최소 서술), 세부 검증 규칙은 링크 |
| 28 §4.7 전체(281-303행) Route 적용·seal 해제·timeout·abort | 04 §7로 3~4문장 압축 + 세션 문서 §8 링크. Diagram의 opt 블록은 04 §4.6 diagram에 최소 형태로 유지(관찰 가능한 상호작용이므로) |
| 28 §7 전체(484-505행) Session owner 검증값 4개 | 04 §7 "요청하는 것은 세 가지" 요약 + 링크 |
| 44 91-94행 Message Follow가 선택 아닌 이유 | 04 §10에 "Session 연결과 중계가 이 전달 경로에 의존한다" 한 문장으로 유지 |
| 44 §4(138-142행) Bound-Actor route update | 04 §4.6·§9 표에 필요한 만큼만 유지, 상세는 링크 |
| 52 §5 전체(197-244행) | 04 §7 요약 + abort 순서는 §4.4 diagram·본문으로 이관(정정 포함) |
| 52 §5 mermaid(204-223행) | 04 §4.6 메인 diagram + §4.4 abort diagram으로 대체 |
| 52 §6 Session owner 행(246-256행) | 04 §7에 요약 |
| 52 §7(258-267행) 1,000ms/3,000ms 값 | 1,000ms는 04 §4.4가 소유, 3,000ms는 세션 문서 §8.1이 소유. 04 §4.6 diagram에 두 값 모두 참고용으로 표기(alt 블록 라벨) |
| 30 590-593행 SessionRelocationSealTimeout | 05 §9·§12에서 세션 문서 링크로 대체 |
| 30 mermaid opt block(636-638, 697-700, 773-775, 843행) | 전부 제거 — 05 §9가 04 §4 링크로 대체되며 unit별 diagram 자체가 삭제됨(mapping §4 S3) |
| 30 §9 표(958행) "Actor에 연결된 session" | 05 §12 표 행을 04 §7·세션 문서 §8 링크로 교체 |
| 30 §10 표(985행) "Session route update 적용" | 05 §13 표 마지막 행을 같은 링크로 교체 |
| 31 §6 전체(191-208행) | 06 §6에 그대로 유지 — 이 절은 Session owner의 재작성/재분배가 아니라 failover 관점의 정책 판단(binding 재사용 금지 등)이므로 원문 유지가 적절하다고 판단, 세션 문서와 내용이 겹치지 않는다(session 문서는 절차, 31/06은 실패 시 정책) |

## 나중에 anchor를 붙일 링크

`01-location-runtime.ko.md`와 `03-relocation-store-redis.ko.md`는 병렬 에이전트가 작성
중이라 파일 링크만 걸고 절 anchor는 비워 뒀다. 아래 위치는 해당 문서가 확정된 뒤 정확한
절 번호로 anchor를 채워야 한다.

| 파일:위치 | 현재 링크 | 채울 anchor(예상 절) |
|---|---|---|
| `04-relocation-flow.ko.md` §10 | `[Location runtime](01-location-runtime.ko.md)` | MessageFollowDuration·최대 hop·순환/generation 결과값을 정의하는 절(mapping §3.2 표 기준 옛 §6.3 대응 절, 새 번호 미정) |
| `05-host-relocation-flow.ko.md` 여러 곳(§1.1 authority·§4·§9) | `[Location runtime](01-location-runtime.ko.md)`, `[Location Store (Redis)](02-location-store-redis.ko.md)`, `[Relocation Store (Redis)](03-relocation-store-redis.ko.md)` | 각각 authority·두 Store 사용 순서 절, provider SPI 절 |
| `06-failure-failover-policy.ko.md` §4.4 | `[Location runtime](01-location-runtime.ko.md)`(최초 message 저장·재개 절) | 옛 §6.1 대응 절 |
| `06-failure-failover-policy.ko.md` §7 | `[Location runtime](01-location-runtime.ko.md)`(Store 응답 유실 절) | 옛 §8 대응 절 |
| `README.ko.md` §5 질문표 4행 | `[01. Location runtime](01-location-runtime.ko.md)`(개요/역할·책임/재생성 구분/Store 연결 차단 절) | 01-location-runtime.ko.md 확정 후 정확한 절 번호로 anchor 추가 |

## 이동 후 갱신할 링크

이 다섯 문서는 옛 경로(`21-location-runtime.ko.md` 등)를 참조하지 않는다 — 모든 관련 내용은
새 문서 안에서 직접 서술하거나 새 문서로 링크했다. 다만 **아직 재구성하지 않은 다른 주제의
문서**는 옛 flat 경로로 링크했다(캠페인 §4.6 규칙 — 아직 아무도 새 경로를 링크하지 않는
단계). 캠페인 마지막 이동 단계(§5)에서 아래 옛 경로 링크를 일괄 치환해야 한다.

| 이 다섯 문서에서 사용한 옛 경로 링크 | 옮겨질 새 경로(추정, topic-map 기준) |
|---|---|
| `../03-interaction-model.ko.md` | 미정(00-foundation 또는 01-execution) |
| `../06-framework-api.ko.md` | 미정 |
| `../15-spot-actor.ko.md` | 03-spot-actor 주제 |
| `../18-object-routing.ko.md` | 03-spot-actor 또는 02-channel-transport 주제 |
| `../20-session-actor-dispatch.ko.md`(직접 링크는 없으나 세션 문서가 소유한다고 서술) | session 주제(이미 재작성됨, 이 다섯 문서는 `04-session/02-session-actor-binding.ko.md`로 이미 새 경로 사용) |
| `../29-transport-liveness.ko.md` | 미정 |
| `../32-framework-error-model.ko.md` | 미정 |
| `../45-internal-routing-and-cache.ko.md`, `../47-internal-object-lifecycle.ko.md`, `../49-internal-liveness-and-state.ko.md`, `../51-internal-service-wire-protocol.ko.md` | internals 문서 — topic-map에서 이 다섯 문서와 같은 주제로 흡수될지 별도 주제로 남을지 미정 |

`06-observability` 주제는 이미 ko 재작성이 완료된 상태(README §8)이고 실제 파일 경로도
확인됐으므로(`06-observability/01-runtime-monitoring.ko.md`,
`02-runtime-metrics.ko.md`, `03-message-flow-tracing.ko.md`), 옛 `24-runtime-monitoring.ko.md`
·`25-runtime-metrics.ko.md`·`26-message-flow-tracing.ko.md` 링크를 이미 새 경로로 갱신했다
— 이 세 링크는 이동 단계에서 다시 손댈 필요가 없다.

## spec-gap 후보

이 세 문서(04·05·06) 재작성에서 새로 발견한 **spec 결함**은 없다. 아래는 문서화 개선
사항으로, 계약 자체를 바꾸지 않는다.

- **용어집에 "Relocation Store" 항목이 없다.** `01-glossary.ko.md`에 "Location Store"(188행)
  항목은 있지만 "Relocation Store" 단독 glossary 항목이 없다 — grep 결과 `### Relocation
  Store` 헤더가 존재하지 않는다. `04-relocation-flow.ko.md`·`05-host-relocation-flow.ko.md`가
  이 용어를 반복 사용하므로(각 문서 §2), 다른 주제가 이미 용어집을 참조하고 있어 이번
  캠페인에서 직접 추가하지 않았다. 용어집 담당 주제(00-foundation)에 항목 추가를 요청할
  spec-gap 후보로 남긴다.
- 30 §7.1의 interruption budget 1초(관측용 warning 임계값)와 04(28)의
  `RelocationCutoverWaitTimeout` 1,000ms(프로토콜 fallback 시한)는 값이 같지만 서로 다른
  개념이라는 점을 `05-host-relocation-flow.ko.md` §8에 명시적으로 밝혔다(mapping이 이미
  구조 문제로 분류, gap 아님 — 처리 완료로 표시).
- 30 §8의 앵커-헤더 불일치와 31 §10의 "정본" 용어는 재작성으로 바로 해소했다(구조 문제,
  §4 S11·S12, gap 아님).
