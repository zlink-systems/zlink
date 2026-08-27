# spot-actor 주제 — 08-routing / 09-object-lifecycle 대장

> 캠페인: [README.ko.md](../../README.ko.md) · 매핑표: [mapping.ko.md](mapping.ko.md)
>
> 산출물: [08-routing.ko.md](../../../../../framework/doc/framework/common/spec/server/03-spot-actor/08-routing.ko.md)
> (18-object-routing 전체 + 45-internal-routing-and-cache §1·§1.1·§2 병합),
> [09-object-lifecycle.ko.md](../../../../../framework/doc/framework/common/spec/server/03-spot-actor/09-object-lifecycle.ko.md)
> (47-internal-object-lifecycle 전체), 그리고 주제 진입 문서
> [README.ko.md](../../../../../framework/doc/framework/common/spec/server/03-spot-actor/README.ko.md).
>
> 병합 경계 한 문장(08-routing 머리말에 그대로 있음): "이 문서는 45의 §1·§1.1·§2(positive
> route cache 자체, resolver 결과 타입, cache 수명, relocation cache 무효화)만 흡수하고,
> 같은 문서의 §3~§7(Channel 대상 선택 알고리즘, 후보 캐시, smooth weighted round-robin,
> 직접 지정 규칙, 발행 fanout)은 02-channel-transport와 12-spot-messaging이 소유하며 원문
> 자리에 그대로 남아 있다."

새 위치 표기: `08-routing.ko.md`의 절은 `08§N`, `09-object-lifecycle.ko.md`의 절은 `09§N`으로
줄여 쓴다.

## 18-object-routing (전량 → 08-routing.ko.md)

| 규칙 ID | 새 위치 | 비고 |
|---|---|---|
| 18-object-routing-R1 | 08§1 | |
| 18-object-routing-R2 | 08§1 | |
| 18-object-routing-R3 | 08§1 | |
| 18-object-routing-R4 | 08§1 | |
| 18-object-routing-R5 | 08§1 | |
| 18-object-routing-R6 | 08§1 | |
| 18-object-routing-R7 | 08§2.1 | |
| 18-object-routing-R8 | 08§2.1 | |
| 18-object-routing-R9 | 08§2.1 | |
| 18-object-routing-R10 | 08§2.1 | |
| 18-object-routing-R11 | 08§2.1 | |
| 18-object-routing-R12 | 08§2.1 | |
| 18-object-routing-R13 | 08§2.1 | |
| 18-object-routing-R14 | 08§2.2 | |
| 18-object-routing-R15 | 08§2.2 | 표 "Cache에 보관하는 정보" |
| 18-object-routing-R16 | 08§2.2 | 표 "사용할 수 있는 기간" |
| 18-object-routing-R17 | 08§2.2 | 표 "기본 설정" |
| 18-object-routing-R18 | 08§2.2 | 표 "저장하지 않는 결과" |
| 18-object-routing-R19 | 08§2.2 | 표 "즉시 무효화하는 조건" |
| 18-object-routing-R20 | 08§2.2 | 표 "Relay 통지" |
| 18-object-routing-R21 | 08§2.2 | 표 "실행 중 설정 변경" |
| 18-object-routing-R22 | 08§2.2 | |
| 18-object-routing-R23 | 08§2.2 | |
| 18-object-routing-R24 | 08§2.2 | |
| 18-object-routing-R25 | 08§2.4 | 옛 §2.3, 병합 후 §2.3에 45§1.1이 새로 들어오며 한 칸 밀림 |
| 18-object-routing-R26 | 08§2.4 | |
| 18-object-routing-R27 | 08§2.4 | |
| 18-object-routing-R28 | 08§2.4 | |
| 18-object-routing-R29 | 08§2.4 | |
| 18-object-routing-R30 | 08§2.5 | 옛 §2.4 |
| 18-object-routing-R31 | 08§2.5 | |
| 18-object-routing-R32 | 08§2.5 | |
| 18-object-routing-R33 | 08§2.5 | |
| 18-object-routing-R34 | 08§2.5 | |
| 18-object-routing-R35 | 08§2.5 | |
| 18-object-routing-R36 | 08§2.5 | 45-R15의 "우회 경로가 닫히기 전에 cache가 먼저 만료" 이유 문장을 이 문장에 이어붙였다 |
| 18-object-routing-R37 | 08§2.5 | |
| 18-object-routing-R38 | 08§2.5 | |
| 18-object-routing-R39 | 08§2.5 | |
| 18-object-routing-R40 | 08§2.5 | |
| 18-object-routing-R41 | 08§2.5 | |
| 18-object-routing-R42 | 08§2.5 | |
| 18-object-routing-R43 | 08§2.5 | |
| 18-object-routing-R44 | 08§2.5 | |
| 18-object-routing-R45 | 08§2.5 | |
| 18-object-routing-R46 | 08§2.5 | |
| 18-object-routing-R47 | 08§2.5 | |
| 18-object-routing-R48 | 08§2.5 | |
| 18-object-routing-R49 | 08§2.5 | |
| 18-object-routing-R50 | 08§2.5 | |
| 18-object-routing-R51 | 08§2.5 | |
| 18-object-routing-R52 | 08§2.6 | 옛 §2.5 |
| 18-object-routing-R53 | 08§2.6 | |
| 18-object-routing-R54 | 08§2.6 | |
| 18-object-routing-R55 | 08§2.6 | 표 행 |
| 18-object-routing-R56 | 08§2.6 | 표 행 |
| 18-object-routing-R57 | 08§2.6 | 표 행 |
| 18-object-routing-R58 | 08§2.6 | 표 행 |
| 18-object-routing-R59 | 08§2.6 | 표 행 |
| 18-object-routing-R60 | 08§2.6 | 표 행 |
| 18-object-routing-R61 | 08§2.6 | 표 행 |
| 18-object-routing-R62 | 08§2.6 | |
| 18-object-routing-R63 | 08§2.6 | |
| 18-object-routing-R64 | 08§2.6 | |
| 18-object-routing-R65 | 08§3.1 | |
| 18-object-routing-R66 | 08§3.1 | |
| 18-object-routing-R67 | 08§3.1 | |
| 18-object-routing-R68 | 08§3.1 | |
| 18-object-routing-R69 | 08§3.1 | 표 |
| 18-object-routing-R70 | 08§3.1 | |
| 18-object-routing-R71 | 08§3.1 | |
| 18-object-routing-R72 | 08§3.1 | |
| 18-object-routing-R73 | 08§3.1 | |
| 18-object-routing-R74 | 08§3.2 | |
| 18-object-routing-R75 | 08§3.2 | |
| 18-object-routing-R76 | 08§3.2 | |
| 18-object-routing-R77 | 08§3.2 | |
| 18-object-routing-R78 | 08§3.2 | |
| 18-object-routing-R79 | 08§3.3 | |
| 18-object-routing-R80 | 08§3.3 | |
| 18-object-routing-R81 | 08§3.3 | 8단계 순서 목록 |
| 18-object-routing-R82 | 08§3.3 | |
| 18-object-routing-R83 | 08§3.3 | |
| 18-object-routing-R84 | 08§3.3 | |
| 18-object-routing-R85 | 08§3.3 | |
| 18-object-routing-R86 | 08§3.3 | |
| 18-object-routing-R87 | 08§3.3 | |
| 18-object-routing-R88 | 08§4.1 | |
| 18-object-routing-R89 | 08§4.1 | |
| 18-object-routing-R90 | 08§4.1 | |
| 18-object-routing-R91 | 08§4.1 | |
| 18-object-routing-R92 | 08§4.2 | |
| 18-object-routing-R93 | 08§4.2 | |
| 18-object-routing-R94 | 08§4.2 | |
| 18-object-routing-R95 | 08§4.2 | |
| 18-object-routing-R96 | 08§4.3 | |
| 18-object-routing-R97 | 08§4.3 | |
| 18-object-routing-R98 | 08§4.3 | |
| 18-object-routing-R99 | 08§4.3 | |
| 18-object-routing-R100 | 08§4.3 | |
| 18-object-routing-R101 | 08§5 | |
| 18-object-routing-R102 | 08§5 | |
| 18-object-routing-R103 | 08§5 | |
| 18-object-routing-R104 | 08§5 | |
| 18-object-routing-R105 | 08§5 | |
| 18-object-routing-R106 | 08§5 | |
| 18-object-routing-R107 | 08§5 | |
| 18-object-routing-R108 | 08§5 | |
| 18-object-routing-R109 | 08§5 | |
| 18-object-routing-R110 | 08§5 | |
| 18-object-routing-R111 | 08§5 | |
| 18-object-routing-R112 | 08§5 | |

## 45-internal-routing-and-cache §1·§1.1·§2 (→ 08-routing.ko.md, §3~§7은 가져오지 않음)

| 규칙 ID | 새 위치 | 비고 |
|---|---|---|
| 45-internal-routing-and-cache-R1 | 08§2.2 | "Message마다 Location Store를 왕복하면… 비용이 모든 호출에서 발생" 문장으로 축약 병합 |
| 45-internal-routing-and-cache-R2 | 08§2.2 | 18-R14와 중복 — 18쪽 표현을 단독 유지, 45 문장은 흡수 |
| 45-internal-routing-and-cache-R3 | 08§2.2 | 새 행 "보관하는 이유" 추가(경로만 캐시하면 낡은 owner로 보내고도 모른다) |
| 45-internal-routing-and-cache-R4 | 08§2.2 | 18-R18과 동일 규칙, 이유 문장만 45쪽에서 흡수 |
| 45-internal-routing-and-cache-R5 | 08§2.2 | 표 "저장하지 않는 결과" 행에 이유 문장으로 병합 |
| 45-internal-routing-and-cache-R6 | 08§2.2 | Resolver 결과 표 도입 문장 |
| 45-internal-routing-and-cache-R7 | 08§2.2 | Resolver 결과 표 `ReadyRoute` 행 |
| 45-internal-routing-and-cache-R8 | 08§2.2 | Resolver 결과 표 `Missing` 행 |
| 45-internal-routing-and-cache-R9 | 08§2.2 | Resolver 결과 표 `Unavailable` 행 |
| 45-internal-routing-and-cache-R10 | 08§2.2 | Resolver 결과 표 `StoreFailure` 행 |
| 45-internal-routing-and-cache-R11 | 08§2.2 | |
| 45-internal-routing-and-cache-R12 | 08§2.2 | |
| 45-internal-routing-and-cache-R13 | 08§2.2 | |
| 45-internal-routing-and-cache-R14 | 08§2.2, 08§2.5 | 세 값 중 앞 두 값(RouteCacheMaxAge, owner 수락 기한)은 §2.2 표, 세 번째(-5초 규칙)는 §2.5(18-R36과 동일 수치) |
| 45-internal-routing-and-cache-R15 | 08§2.5 | "우회 경로가 닫히기 전에 cache가 먼저 만료" 이유 문장을 18-R36 문장에 이어붙였다 |
| 45-internal-routing-and-cache-R16 | 08§2.3 | 신설 절(45§1.1 전체) |
| 45-internal-routing-and-cache-R17 | 08§2.3 | |
| 45-internal-routing-and-cache-R18 | 08§2.3 | |
| 45-internal-routing-and-cache-R19 | 08§2.3 | |
| 45-internal-routing-and-cache-R20 | 08§2.3 | |
| 45-internal-routing-and-cache-R21 | 08§2.3 | |
| 45-internal-routing-and-cache-R22 | 08§2.3 | |
| 45-internal-routing-and-cache-R23 | 08§2.5 | "이동과 cache가 만나는 지점 — 성능 절벽" 단락 |
| 45-internal-routing-and-cache-R24 | 08§2.5 | |
| 45-internal-routing-and-cache-R25 | 08§2.5 | |
| 45-internal-routing-and-cache-R26 | 08§2.5 | |
| 45-internal-routing-and-cache-R27 | 08§2.5 | |
| 45-internal-routing-and-cache-R28 | 08§2.5 | |
| 45-internal-routing-and-cache-R29 | 08§2.5 | |
| 45-internal-routing-and-cache-R30 | 08§2.5 | |
| 45-internal-routing-and-cache-R31 | 08§2.5 | wire 형식 문단 |
| 45-internal-routing-and-cache-R32 | 08§2.5 | |
| 45-internal-routing-and-cache-R33 | 08§2.5 | |
| 45-internal-routing-and-cache-R34 | 08§2.5 | |
| 45-internal-routing-and-cache-R35 | 08§2.5 | registry key 문단 |
| 45-internal-routing-and-cache-R36 | 08§2.5 | |
| 45-internal-routing-and-cache-R37 | 08§2.5 | stateDiagram-v2 + 문장 |
| 45-internal-routing-and-cache-R38 | 08§2.5 | |
| 45-internal-routing-and-cache-R39 | 08§2.5 | |
| 45-internal-routing-and-cache-R40 | 08§2.5 | |
| 45-internal-routing-and-cache-R41 | 08§2.5 | |
| 45-internal-routing-and-cache-R42 | 08§2.5 | |
| 45-internal-routing-and-cache-R43~R96 | (가져오지 않음) | §3~§7. 아래 "가져오지 않은 45 절" 참고 |
| 45-internal-routing-and-cache-R97 | 08§5 | 18-R102(Cache hit 시 Store 미조회)와 동일 규칙, 중복 흡수 |
| 45-internal-routing-and-cache-R98 | 08§5 | 18-R103(negative cache 금지)과 동일 규칙, 중복 흡수 |
| 45-internal-routing-and-cache-R99 | 08§5 | 신설 불릿 "Resolver 결과가 Missing과 Unavailable을 서로 다른 tag로…" |
| 45-internal-routing-and-cache-R100 | 08§5 | 위와 같은 불릿에 이어 서술 |
| 45-internal-routing-and-cache-R101 | 08§5 | 신설 불릿 "Positive route cache의 수명이 MessageFollowDuration을 넘지 않는다" |
| 45-internal-routing-and-cache-R102 | 08§5 | 신설 불릿 "유효한 messageFollow 통지를 받으면… 즉시 무효화" |
| 45-internal-routing-and-cache-R103~R112 | (가져오지 않음) | §3~§7 검증 항목. 아래 "가져오지 않은 45 절" 참고 |

## 47-internal-object-lifecycle (전량 → 09-object-lifecycle.ko.md)

| 규칙 ID | 새 위치 | 비고 |
|---|---|---|
| 47-internal-object-lifecycle-R1 | 09§1 | |
| 47-internal-object-lifecycle-R2 | 09§1 | 표 행 |
| 47-internal-object-lifecycle-R3 | 09§1 | 표 행 |
| 47-internal-object-lifecycle-R4 | 09§1 | 표 행 |
| 47-internal-object-lifecycle-R5 | 09§1 | "결정 —" 라벨 제거, 굵은 규칙 문장으로 |
| 47-internal-object-lifecycle-R6 | 09§1 | "언어별 재량 — 표현 방법" |
| 47-internal-object-lifecycle-R7 | 09§2 | |
| 47-internal-object-lifecycle-R8 | 09§2 | |
| 47-internal-object-lifecycle-R9 | 09§2 | |
| 47-internal-object-lifecycle-R10 | 09§3 | |
| 47-internal-object-lifecycle-R11 | 09§3 | "Spec 상태를 activation state machine에 전달한다" 표 |
| 47-internal-object-lifecycle-R12 | 09§3 | |
| 47-internal-object-lifecycle-R13 | 09§3 | |
| 47-internal-object-lifecycle-R14 | 09§3 | |
| 47-internal-object-lifecycle-R15 | 09§3 | |
| 47-internal-object-lifecycle-R16 | 09§3 | "동시에 만들려 할 때" |
| 47-internal-object-lifecycle-R17 | 09§3 | "결정 —" 라벨 제거, 08-routing §2.2로 링크 갱신 |
| 47-internal-object-lifecycle-R18 | 09§3 | mermaid sequenceDiagram |
| 47-internal-object-lifecycle-R19 | 09§3 | "Ready 기록과 대상 route의 공개 순서" |
| 47-internal-object-lifecycle-R20 | 09§3 | |
| 47-internal-object-lifecycle-R21 | 09§3 | |
| 47-internal-object-lifecycle-R22 | 09§3 | |
| 47-internal-object-lifecycle-R23 | 09§3 | |
| 47-internal-object-lifecycle-R24 | 09§3 | |
| 47-internal-object-lifecycle-R25 | 09§3 | "만들다 실패하면" |
| 47-internal-object-lifecycle-R26 | 09§4 | |
| 47-internal-object-lifecycle-R27 | 09§4 | "결정 —" 라벨 제거 |
| 47-internal-object-lifecycle-R28 | 09§4 | |
| 47-internal-object-lifecycle-R29 | 09§4 | 표 |
| 47-internal-object-lifecycle-R30 | 09§4 | "결정 —" 라벨 제거 |
| 47-internal-object-lifecycle-R31 | 09§4 | |
| 47-internal-object-lifecycle-R32 | 09§5 | "쓰지 않고 남아 있는…" — S14 어휘 교체("유휴"→"쓰지 않고 남아 있는") |
| 47-internal-object-lifecycle-R33 | 09§5 | |
| 47-internal-object-lifecycle-R34 | 09§5 | |
| 47-internal-object-lifecycle-R35 | 09§5 | |
| 47-internal-object-lifecycle-R36 | 09§5 | |
| 47-internal-object-lifecycle-R37 | 09§5 | |
| 47-internal-object-lifecycle-R38 | 09§5 | |
| 47-internal-object-lifecycle-R39 | 09§5 | |
| 47-internal-object-lifecycle-R40 | 09§5 | |
| 47-internal-object-lifecycle-R41 | 09§5 | |
| 47-internal-object-lifecycle-R42 | 09§5 | |
| 47-internal-object-lifecycle-R43 | 09§5 | "상한은 배치 단계뿐 아니라 로컬 활성화에도 적용한다" — 소제목 원문 오류("있지만…쓴다") 정정, 내용 동일 |
| 47-internal-object-lifecycle-R44 | 09§5 | 표, "결정 —" 라벨 제거 |
| 47-internal-object-lifecycle-R45 | 09§5 | |
| 47-internal-object-lifecycle-R46 | 09§5 | "결정 —" 라벨 제거 |
| 47-internal-object-lifecycle-R47 | 09§5 | |
| 47-internal-object-lifecycle-R48 | 09§5 | |
| 47-internal-object-lifecycle-R49 | 09§5 | |
| 47-internal-object-lifecycle-R50 | 09§5 | "결정 —" 라벨 제거 |
| 47-internal-object-lifecycle-R51 | 09§5 | "결정 —" 라벨 제거 |
| 47-internal-object-lifecycle-R52 | 09§5 | |
| 47-internal-object-lifecycle-R53 | 09§6 | |
| 47-internal-object-lifecycle-R54 | 09§6 | |
| 47-internal-object-lifecycle-R55 | 09§6 | |
| 47-internal-object-lifecycle-R56 | 09§6 | |
| 47-internal-object-lifecycle-R57 | 09§6 | |
| 47-internal-object-lifecycle-R58 | 09§6 | |
| 47-internal-object-lifecycle-R59 | 09§6 | |
| 47-internal-object-lifecycle-R60 | 09§6 | "결정 —" 라벨 제거 |
| 47-internal-object-lifecycle-R61 | 09§6 | |
| 47-internal-object-lifecycle-R62 | 09§6 | "결정 —" 라벨 제거 |
| 47-internal-object-lifecycle-R63 | 09§6 | |
| 47-internal-object-lifecycle-R64 | 09§6 | |
| 47-internal-object-lifecycle-R65 | 09§6 | |
| 47-internal-object-lifecycle-R66 | 09§6 | 표 |
| 47-internal-object-lifecycle-R67 | 09§6 | |
| 47-internal-object-lifecycle-R68 | 09§6 | "결정 —" 라벨 제거 |
| 47-internal-object-lifecycle-R69 | 09§6 | |
| 47-internal-object-lifecycle-R70 | 09§6 | |
| 47-internal-object-lifecycle-R71 | 09§6 | |
| 47-internal-object-lifecycle-R72 | 09§8 | 옛 §7(확인할 결과), 새 문서에서는 §8로 밀림 |
| 47-internal-object-lifecycle-R73 | 09§8 | |
| 47-internal-object-lifecycle-R74 | 09§8 | |
| 47-internal-object-lifecycle-R75 | 09§8 | |
| 47-internal-object-lifecycle-R76 | 09§8 | |
| 47-internal-object-lifecycle-R77 | 09§8 | |
| 47-internal-object-lifecycle-R78 | 09§8 | |
| 47-internal-object-lifecycle-R79 | 09§8 | |
| 47-internal-object-lifecycle-R80 | 09§8 | |
| 47-internal-object-lifecycle-R81 | 09§8 | |
| 47-internal-object-lifecycle-R82 | 09§8 | |
| 47-internal-object-lifecycle-R83 | 09§8 | |
| 47-internal-object-lifecycle-R84 | 09§8 | |
| 47-internal-object-lifecycle-R85 | 09§8 | |
| 47-internal-object-lifecycle-R86 | 09§8 | |
| 47-internal-object-lifecycle-R87 | 09§8 | |
| 47-internal-object-lifecycle-R88 | 09§8 | |
| 47-internal-object-lifecycle-R89 | 09§8 | |
| 47-internal-object-lifecycle-R90 | 09§8 | |
| 47-internal-object-lifecycle-R91 | 09§8 | |
| 47-internal-object-lifecycle-R92 | 09§8 | |
| 47-internal-object-lifecycle-R93 | 09§7 | 옛 문서의 번호 없는 마지막 절 "Object queue와 host shared capacity"를 §7 "다른 주제와의 경계"로 번호를 붙여 흡수(S15) |
| 47-internal-object-lifecycle-R94 | 09§7 | 01-execution 주제 문서(`05-application-job-queue-and-backpressure.ko.md`) 링크 추가 |

## 가져오지 않은 45 절

45-internal-routing-and-cache.ko.md의 §3~§7은 이번 재작성에서 손대지 않고 원 위치에 그대로
둔다. 마지막 단계(en 작성 + 이동)에서 아래 주제가 처리한다.

| 45의 절 | 내용 | 흡수해야 할 주제 |
|---|---|---|
| §3 후보 목록을 호출마다 만들지 않는다 | Channel select-one 후보 캐시 | 02-channel-transport(08-channel-messaging) |
| §4 선택을 어느 계층이 하는가 | MeshNode·ClientServer·수동 fallback 선택 계층, connection 관리 경계 | 02-channel-transport(08-channel-messaging, 07-channel-topology) |
| §5 선택 알고리즘을 지정한다 | Smooth weighted round-robin 절차, tiebreak, 주기 캐싱 | 02-channel-transport(08-channel-messaging, 09-client-server-channel) |
| §6 직접 지정한 대상은 바꾸지 않는다 | 대상 직접 지정 호출 일반 규칙 | 02-channel-transport |
| §7 여러 대상에게 함께 보낼 때 | Logical Multicast/publish fanout, node당 wire record 1개, 부분 실패 미복구 | 02-channel-transport(08-channel-messaging)와 12-spot-messaging §4(완료 계약은 12-spot-messaging이 이미 소유 — S12) |
| §8 확인할 결과(§3~§7분) | 위 각 절의 검증 요구 | 절 분할과 같이 나뉜다 |

## 나중에 anchor를 붙일 링크

주제 README([README.ko.md](../../../../../framework/doc/framework/common/spec/server/03-spot-actor/README.ko.md))
§4·§5는 아직 작성되지 않은 다른 7개 문서(01·02·03·04·05·06·07)를 파일로만 링크했다. 그
문서들이 모두 작성되면 다음 자리에 절 anchor를 붙여야 한다.

- README §4 "이 주제의 문서" 표의 7개 문서 링크 — 지금은 파일 링크만, 각 문서 §1 개요
  절로 anchor를 붙인다.
- README §5 "질문으로 찾기" 표에서 01·02·03·04·05·06·07을 가리키는 행 — 지금은 파일
  링크만, 실제 답이 있는 절 번호로 anchor를 붙인다(예: "Actor는 무엇이고 어떻게
  identity·queue·control을 갖는가" → `04-actor-model.ko.md#N-...`).
- README §3 그림 아래 문장의 `05-spot-actor-membership.ko.md` 링크 — 문서가 작성되면 join
  순서를 다루는 정확한 절로 anchor를 붙인다.
- `08-routing.ko.md` §2.5의 `05-spot-actor-membership.ko.md#42-...` 링크는 이미 절 번호를
  지정했다(옛 15 §4.2와 같은 번호를 가정). `05-spot-actor-membership.ko.md`가 실제로
  작성되면 그 문서의 §4.2가 같은 내용("다른 node의 Spot으로 Actor를 join하는 순서")을
  다루는지, 절 번호가 같은지 재확인해야 한다.

## 이동 후 갱신할 링크

08-routing.ko.md와 09-object-lifecycle.ko.md는 아직 옮기지 않은 root `spec/server/` 문서를
옛 경로(`../NN-slug.ko.md`)로 링크한다. 그 문서들이 각자의 새 주제 디렉터리로 이동하면 아래
링크의 상대 경로와 anchor를 함께 갱신해야 한다.

| 링크 | 현재 경로 | 이동 예정 주제(추정) |
|---|---|---|
| `../01-glossary.ko.md` (다수) | root | 00-foundation |
| `../07-channel-topology.ko.md` | root | 02-channel-transport |
| `../03-interaction-model.ko.md#10-handler-실패` | root | 00-foundation |
| `../06-framework-api.ko.md` | root | 00-foundation |
| `../11-spot-model.ko.md#42-...`, `#62-유휴-instance-spot-정리` | root | 03-spot-actor(01-spot-model) — 같은 주제 안이지만 아직 작성 전이라 옛 경로. `01-spot-model.ko.md`가 작성되면 anchor(특히 "유휴" 표현이 그 문서에서도 바뀌면 slug 자체가 변함)를 재확인 |
| `../28-relocation-flow.ko.md` | root | 05-location-relocation(추정) |
| `../30-host-relocation-flow.ko.md#9-...` | root | 05-location-relocation(추정) |
| `../31-failure-failover-policy.ko.md#44-...`, `#2-...` | root | 05-location-relocation(추정) |
| `../32-framework-error-model.ko.md` | root | 00-foundation(추정) |
| `../41-internal-serialization.ko.md#2-...` | root | 01-execution(추정) |
| `../46-internal-dispatch-loop.ko.md` | root | 01-execution(추정) |
| `../50-internal-message-ownership.ko.md` | root | 01-execution(추정) |
| `../01-execution/04-application-job-queue-and-backpressure.ko.md` | 이미 새 경로로 링크(01-execution 진행 중) | 01-execution 작업자가 실제 문서·절 번호를 확정하면 이 링크의 anchor·파일명을 재확인 |
| `45-internal-routing-and-cache.ko.md` (08-routing 머리말의 "45 원문" 링크) | root, 그대로 남는 파일 | §3~§7이 02-channel-transport로 옮겨지면 이 링크가 가리키는 문서 자체가 없어지거나 성격이 바뀔 수 있다 — 이동 단계에서 재확인 |

## spec-gap 후보

이 두 문서를 재작성하면서 새로 발견한 실제 스펙 결함 후보는 없다. mapping.ko.md의 G1·G2·G3은
14-actor-model/15-spot-actor(G1), 4언어 구현 대조 필요(G2), 12/16의 단계 수 차이(G3)에
관한 것으로 이 두 문서(18, 45§1·§1.1·§2, 47)의 재작성 범위 밖이다 — [spec-gap 대장](../../spec-gap.ko.md)
등록은 해당 주제 판정 단계(§4.5)에서 처리한다.

---

[스펙 목차](../../../../../framework/doc/framework/common/spec/server/README.ko.md) ·
[캠페인 지침](../../README.ko.md) · [매핑표](mapping.ko.md)
