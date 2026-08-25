# 01-execution — ledger `04-spot-timer` · `06-payload-ownership-and-codec`

> 대상: [`04-spot-timer.ko.md`](../../../../../framework/doc/framework/common/spec/server/03-spot-actor/10-spot-timer.ko.md) ·
> [`06-payload-ownership-and-codec.ko.md`](../../../../../framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md)。
> 옛 출처: `05-async-execution-policy.ko.md` §5(R39–R45 중 timer 관련 R39–R43) ·
> `46-internal-dispatch-loop.ko.md` §7(R80–R83 중 timer 관련 R80–R82, R84 일부) ·
> `50-internal-message-ownership.ko.md` 전체(R132–R155). 양식은
> [mapping.ko.md](mapping.ko.md) §5를 따른다. "새 위치"는 새 문서에서 grep으로 핵심 문구를
> 실물 확인한 뒤에만 채웠다(가이드 §2.5).

## R# → 새 위치

| R# | 새 위치 | 비고 |
|---|---|---|
| R39 | `04-spot-timer.ko.md` §1 Timer generation과 cancel | generation 증가·이전 generation 미실행·cancel 차단 범위·동시 실행 금지 규칙 표 포함 그대로 이동 |
| R40 | `04-spot-timer.ko.md` §2 Overrun policy | 3종 policy 표, `MaxCatchUpTicks` 기본값·범위, relocation encoding normalize 문장 포함 |
| R41 | `04-spot-timer.ko.md` §1 Timer generation과 cancel | tick 정보 필드(`DeliveryIndex`·`ScheduledIndex`·`SkippedTicks`) 의미와 계산식, wall-clock 비공개 문장 포함. 매핑표는 §1 소유로 명시(§3.5) |
| R42 | `04-spot-timer.ko.md` §3 Owner lease와 admission | owner lease·admission deadline 확인, monotonic deadline 초과 시 재개 후 미투입, 이전 owner authority pending tick 미실행 |
| R43 | `04-spot-timer.ko.md` §7 고빈도 timer의 batch 처리 | 05 §5와 46 §7의 같은 문장 중복을 통합해 한 절에만 서술(mapping S 규칙 — "중복, 46으로 흡수") |
| R80 | `04-spot-timer.ko.md` §4 공유 scheduler — 자원은 등록 수에 비례하지 않는다 | 비교표(전용 자원 2만 개 vs 공유 scheduler+대기열) 그대로 이동. 자원 수 비례 여부는 §4 끝에 "내부 확인 조건"으로 부기(공개 표면 비관찰) |
| R81 | `04-spot-timer.ko.md` §5 늦은 tick 처리의 내부 구현 | 기본 병합·따라잡기 상한·tick 통계 무한 누적 금지. 통계 누적 금지는 §5 끝에 "내부 확인 조건"으로 부기 |
| R82 | `04-spot-timer.ko.md` §6 Tick이 실행 권한으로 들어가는 경로 | `SpotWide` 공유 권한/`PerActor` timer 이름별 권한, 권한 미획득 시 재시도. 권한 자체의 규칙은 `02-handler-turn-and-execution-gate.ko.md` 링크로 대체(mapping 지정) |
| R84(timer 관련 부분) | `04-spot-timer.ko.md` §8 검증 요구 + §4·§5 "내부 확인 조건" | 46 §9의 timer 관련 행 중 공개 표면 관찰 가능한 것(overrun 결과·owner lease 결과)은 §8로, 자원 수·메모리 사용량처럼 내부 계측이 필요한 것은 규칙 문단의 "내부 확인 조건"으로 분리(가이드 §4.4·§9.3, S11) |
| R132 | `06-payload-ownership-and-codec.ko.md` §1 두 종류의 복사를 구분한다 | binding 강제 복사(제거 불가) vs framework 추가 복사(목표 0) 구분, view 미제공 가능 문장 포함 |
| R133 | `06-payload-ownership-and-codec.ko.md` §2 없앨 수 있는 복사 | 5유형 표(경계 왕복·접근자 복사·큐 넘기려는 복사·이중 보관·미리 만드는 이동 기록) 그대로 이동 |
| R134 | `06-payload-ownership-and-codec.ko.md` §3 이동 기록을 hot path에서 만들지 않는다 | 4벌 동시 상주 가능성, 봉인 뒤 생성 규칙, "원인은 원본 해제 시점" 문장 포함 |
| R135 | `06-payload-ownership-and-codec.ko.md` §4 큐에 있는 동안의 소유자 | framework 소유·handler 종료 후 해제 규칙, 무엇을 해제하는지는 binding 표현에 따름 |
| R136 | `06-payload-ownership-and-codec.ko.md` §4 큐에 있는 동안의 소유자 | 소유권 전이 단방향 규칙(binding storage→framework storage→handler value), 상태 그림(§4) |
| R137 | `06-payload-ownership-and-codec.ko.md` §4 큐에 있는 동안의 소유자 | 모든 terminal 경로가 같은 release 지점으로 모임, `close`/`Dispose`·GC 위임 언어의 논리적 release 지점 |
| R138 | `06-payload-ownership-and-codec.ko.md` §5 Handler에 무엇을 넘기는가 | 역직렬화된 소유 객체만 전달, native 저장소·해제 책임 미전달, 역직렬화 1회 불가피성 |
| R139 | `06-payload-ownership-and-codec.ko.md` §5 Handler에 무엇을 넘기는가 | 복사 회계 3종 분리(buffer 전체 복사/view·slice/객체 생성), "역직렬화 1회는 buffer 복사 축" 문장 |
| R140 | `06-payload-ownership-and-codec.ko.md` §5 Handler에 무엇을 넘기는가 | 불변 payload 이중 복사 위험, raw byte API는 transport 검사·codec extension 전용 |
| R141 | `06-payload-ownership-and-codec.ko.md` §6 역직렬화를 언제 하는가 | 헤더 우선 읽기·역직렬화는 실행 권한 획득 후 handler 직전 규칙 |
| R142 | `06-payload-ownership-and-codec.ko.md` §6 역직렬화를 언제 하는가 | 미수락 message 역직렬화 금지, Application job queue 포화는 이 거부가 아니라는 구분 |
| R143 | `06-payload-ownership-and-codec.ko.md` §6 역직렬화를 언제 하는가 | Relocation seal 뒤 도착 message 비거부, target handoff/Message Follow로 이관, 역직렬화 지연 |
| R144 | `06-payload-ownership-and-codec.ko.md` §6 역직렬화를 언제 하는가 | 실행 권한 전 역직렬화 시 거절된 message에도 비용 발생, 형식 판별 이중 해석 금지 |
| R145 | `06-payload-ownership-and-codec.ko.md` §6 역직렬화를 언제 하는가 | typed payload 최대 1회 역직렬화, 저장된 값/실패 재사용, type mismatch·raw view 예외 |
| R146 | `06-payload-ownership-and-codec.ko.md` §7 Codec 선택 — 계약과 internals의 경계 | 여러 serializer 동시 등록 전제, API 모양 언어별 상이, 송신/수신 fallback 차이 표. 계약 소유는 `06-framework-api.ko.md#9-codec` 링크로 명시 |
| R147 | `06-payload-ownership-and-codec.ko.md` §7 Codec 선택 — 계약과 internals의 경계 | 송신 selector는 선언 type(나중 등록 우선), 수신은 정규화된 content-type 정확 비교·`ProtocolError` |
| R148 | `06-payload-ownership-and-codec.ko.md` §7 Codec 선택 — 계약과 internals의 경계 | registry·codec 선택 cache·dispatch 구현은 비계약, 선택 사실만 계약 |
| R149 | `06-payload-ownership-and-codec.ko.md` §7 Codec 선택 — 계약과 internals의 경계 | message마다 반복 금지 4행 표(조회+객체 생성/content-type 문자열화/후보 배열/기본형식 비교) |
| R150 | `06-payload-ownership-and-codec.ko.md` §7 Codec 선택 — 계약과 internals의 경계 | registry 불변 → 선택 결과 캐시, 송신/수신 캐시 키 차이 표, 잠금 없이 읽기 근거 |
| R151 | `06-payload-ownership-and-codec.ko.md` §7 Codec 선택 — 계약과 internals의 경계 | 송신 캐시 1,024개 상한, 초과 시 기존 entry 미제거, 신규 type 미저장 |
| R152 | `06-payload-ownership-and-codec.ko.md` §7 Codec 선택 — 계약과 internals의 경계 | content-type 비교 문자열 재생성 금지, 후보 목록·임시 객체 재생성 금지, JSON fallback 금지 |
| R153 | `06-payload-ownership-and-codec.ko.md` §7 Codec 선택 — 계약과 internals의 경계 | 수신 content-type이 선택 필수 입력이라는 문장(전달용 metadata 아님) |
| R154 | `06-payload-ownership-and-codec.ko.md` §8 Retained Core lease와 1:N child 소유권 | Core lease shared owner, child callback 첫 instruction에서 permit 반환, lazy 확보, 무제한 복제 큐 금지 |
| R155 | `06-payload-ownership-and-codec.ko.md` §8 Retained Core lease와 1:N child 소유권 | 모든 child terminal 뒤 Core lease 1회 반환, partial acquire 중 실패 시 정리 규칙, one-way record의 record terminal 정의 |
| — (50 §8 확인할 결과 20항) | `06-payload-ownership-and-codec.ko.md` §9 검증 요구 | R132–R155에 이미 반영된 규칙의 관찰 결과이므로 별도 R 번호 없이 §9로 흡수(mapping 지정). 공개 표면으로 관찰 불가능한 복사 횟수 항목은 §9 "내부 확인 조건" 하위로 분리 |

배치하지 못한 R#: 없음. R39–R43, R80–R82, R84(timer 관련), R132–R155 전부 새 문서에서 grep으로
실물 확인했다.

## 이동 후 갱신할 링크

두 문서가 사용하는 링크 중 아직 이동하지 않은 옛 경로(캠페인 §5 마지막 이동 단계에서
새 경로로 함께 갱신 대상):

| 링크 | 현재 문서·위치 | 이동 시 처리 |
|---|---|---|
| `../01-glossary.ko.md#spot`, `#spot-turn`, `#owner-lease` | `04-spot-timer.ko.md` 서문·§3 | 용어집이 `00-foundation/`으로 이동하면 상대 경로 갱신(§5 anchor 치환표 대상) |
| `../01-glossary.ko.md#object-execution-queue`, `#application-job-queue` | `06-payload-ownership-and-codec.ko.md` §4·§6 | 위와 동일 |
| `../06-framework-api.ko.md`, `../06-framework-api.ko.md#9-codec` | `06-payload-ownership-and-codec.ko.md` 서문·§7(2회) | `06-framework-api`가 속할 주제(00-foundation 추정)로 이동하면 경로·anchor 갱신 |
| `../08-channel-messaging.ko.md` | `06-payload-ownership-and-codec.ko.md` 서문 | 해당 문서가 속할 주제로 이동하면 경로 갱신 |
| `../46-internal-dispatch-loop.ko.md` | `06-payload-ownership-and-codec.ko.md` §8 | 46은 이번 `01-execution` 주제 안에서 `02-handler-turn-and-execution-gate.ko.md`·`05-application-job-queue-and-backpressure.ko.md`로 흩어질 예정(mapping §3). 두 문서가 재작성되면 이 링크를 해당 새 문서·절 anchor로 갱신 |

내부(같은 새 topic 디렉터리) 링크는 이미 새 파일명을 사용한다 — `README.ko.md`,
`02-handler-turn-and-execution-gate.ko.md`, `03-cancellation-and-shutdown.ko.md`,
`05-application-job-queue-and-backpressure.ko.md`. 이 문서들이 아직 작성되지 않았더라도
mapping §3이 고정한 파일명이므로 갱신 대상이 아니다.

## spec-gap 후보

이번 두 문서 재작성에서 새로 발견한 spec-gap 후보는 없다. mapping.ko.md §5의 G1–G3은 이
두 문서가 다루는 R행과 직접 겹치지 않으므로(G1은 43, G2는 41·46 §4, G3은 41·42) 여기서는
반복하지 않는다.

---

[01-execution 매핑표](mapping.ko.md) · [캠페인 지침](../../README.ko.md)
