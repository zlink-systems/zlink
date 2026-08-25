# observability 주제 — 03/04 재작성 대장

> [매핑표](mapping.ko.md) §5의 R51–R99를 재작성 뒤 실물 grep으로 확인하고 새 위치를
> 채운 대장이다. 대상: `06-observability/03-message-flow-tracing.ko.md`,
> `06-observability/04-flow-correlation.ko.md`, `06-observability/README.ko.md`.

## R# → 새 위치

| R# | 새 위치 | 비고 |
|---|---|---|
| R51 | `03-message-flow-tracing.ko.md` §1 | 24/25/27과의 소유 경계 문장은 `README.ko.md` §2 표로 옮기고, 본문은 §3.0 대신 §2 링크로 축소(계획대로) |
| R52 | `03` §2 | |
| R53 | `03` §2.1 | phase 9값 + sequence diagram |
| R54 | `03` §2.1 | |
| R55 | `03` §2.2 | 6그룹 목록 + wrapper/transport 중복 금지 |
| R56 | `03` §3.1 | 닫힌집합 5종 표 + `shutdown` 의미 |
| R57 | `03` §3.1 | `zlink.message_flow`의 `reason` 7값 |
| R58 | `03` §3.1 | `zlink.dispatch_error`의 `reason` 9값 + `action` 3값 |
| R59 | `03` §3.2 | attribute 포함 조건 표 17항목 |
| R60 | `03` §3.2 | |
| R61 | `03` §3.2 「Structured log 대체 표기」 | 소제목으로 분리(mapping S8) |
| R62 | `03` §3.2 「Structured log 대체 표기」 아래 | |
| R63 | `03` §4 | diagnostics level 4값 표 + 기본값 `Errors` |
| R64 | `03` §4 | sampling rate 규칙 |
| R65 | `03` §4 | 비규범적 C# 발췌 |
| R66 | `03` §5 | 실행 중 level 변경 규칙 |
| R67 | `03` §5 | `Off` 비용 규칙(7개 불릿) |
| R68 | `03` §6 | |
| R69 | `03` §6 | |
| R70 | `03` §6 | |
| R71 | `03` §6 | |
| R72 | `04-flow-correlation.ko.md` §1 | |
| R73 | `04` §2 | 역할 표 + `flow_id` 용도 제한 |
| R74 | `04` §2 | downstream request sequence diagram |
| R75 | `04` §3 | 형식 표 3행 |
| R76 | `04` §3 | protocol error 조건 |
| R77 | `04` §4 | flow 생성 시점 3항목 |
| R78 | `04` §4 | `Off`일 때 관측 전용 처리 생략 |
| R79 | `04` §4 | `correlation_id`는 `Off`에도 유지 |
| R80 | `04` §4 | flow context 설정/복원, `Off`에서 미생성. `03` §5 링크 |
| R81 | `04` §5 | 처리 경계 보존 범위 — `03` §2.2를 표준 목록으로 링크(mapping S14). 표를 다시 만들지 않고 산문으로 대표 예만 언급 |
| R82 | `04` §5 | Logical Multicast·Classic fanout `flow_id` 공유 |
| R83 | `04` §5 | 중간 runtime downstream `correlation_id` |
| R84 | `04` §5 | Instance Spot 생성 권한 실패 시 1회 재전달 |
| R85 | `04` §6 | |
| R86 | `04` §7 | |
| R87 | `04` §7 | |
| R88 | `04` §7 | |
| R89 | `04` §7 | |
| R90 | `04` §8 | |
| R91 | (24 소유, 이번 재작성 대상 아님) | `runtime-monitoring` 재작성 담당이 반영 |
| R92 | (25 소유, 이번 재작성 대상 아님) | `runtime-metrics` 재작성 담당이 반영 |
| R93 | `03` §6 + §7 검증 요구 | "Instance Spot의 one-way 생성 실패는 …정확히 한 번 기록" 문장을 본문(§6)과 검증 요구(§7)에 함께 배치 |
| R94 | `03` §7 검증 요구, `04` §9 검증 요구 | "지나간 처리 단계에 되돌려 적용하지 않는다" 확인 항목을 두 문서 검증 요구 절에 각각 배치(§4/§4.1 소급 금지 문장과 짝) |
| R95 | `README.ko.md` §4.1 | 무엇을 먼저 켜는가 표 4행 + 첫 재현부터 서버 log 보존 |
| R96 | `README.ko.md` §4.2 | 어떻게 읽는가 |
| R97 | `README.ko.md` §4.3 | 실패는 반드시 flow에 남긴다. `message_flow_outcome`·`errorType`·`errorMessage` 세 이름은 새로 만들지 않고 `03`이 실제로 정의하는 `outcome=failed`/`reason`/구현이 정한 길이 제한 안의 error 설명 문자열로 다시 씀(spec-gap G3으로 근본 원인 등록, 아래 spec-gap 후보 참고) |
| R98 | `03` §5 | Trace 비용 규칙 — hot path `if (enabled(outcome))`, lazy 형태, hot path에서 lazy도 `if`로 재차 감싸기, gate 앞 문자열 연결 금지. `**결정**` 라벨 없이 굵은 규칙 문장으로 씀 |
| R99 | `03` §5 「언어별 재량」 | gate 표현 방법(C++ template lambda/.NET 보간 문자열+`Func<>`/Java `Supplier<>`/Node thunk), 관찰 결과가 같은 이유(비용 0)와 확인 기준(tracing 끈 상태에서 문자열·event·lambda 미생성을 호출부 코드로 확인)을 함께 적음 |

R95–R99는 지시대로 모두 `06-observability/README.ko.md`에 배치했다(옛 최상위
`README.ko.md` "디버깅 원칙"·"Trace 비용 규칙" 절 이관). R98·R99는 옛 "Trace 비용
규칙"과 26 §4.1(R66·R67)이 사실상 같은 zero-cost 요구를 다른 표현으로 반복하던
것을 `03` §5 한 곳으로 합쳐 썼다(mapping S9) — README §4.3은 그 절로 가는 링크만
남긴다.

빈 칸(누락) 0건, 대장에 없는 추가 보장 0건.

## 이동 후 갱신할 링크

캠페인 §5(최종 이동 단계)에서 처리할 옛 경로 참조. 이번 작업은 옛 문서를 손대지
않는 원칙(campaign README §4.6)을 따랐으므로 아래는 기록만 하고 고치지 않았다.

- `framework/doc/framework/common/spec/server/README.ko.md` — 목차 행 99~100(`26-message-flow-tracing.ko.md`,
  `27-flow-correlation.ko.md`) + "여러 장이 연결되는 구조 결정" 표의 24 링크,
  그리고 §227~273 "디버깅 원칙"·"Trace 비용 규칙" 절 전체(이 두 절은 삭제하고
  `06-observability/README.ko.md`로 가는 한 줄 링크로 대체)
- `24-runtime-monitoring.ko.md:22,347` — `26-message-flow-tracing.ko.md` 참조
- `25-runtime-metrics.ko.md` — `26-message-flow-tracing.ko.md` 참조(§1 서문, §5, §9)
- `23-relocation-store-redis.ko.md:63` — `26-message-flow-tracing.ko.md` 참조
- `30-host-relocation-flow.ko.md:488` — `26-message-flow-tracing.ko.md` 참조
- `06-framework-api.ko.md:828` — `26-message-flow-tracing.ko.md#3-공통-attribute` anchor 참조(절 번호는 유지되므로 anchor 자체는 안 바뀜, 경로만 치환)
- `04-message-model.ko.md:152` — `27-flow-correlation.ko.md` 참조
- `01-glossary.ko.md:691` — `27-flow-correlation.ko.md` 참조
- `49-internal-liveness-and-state.ko.md:214` — `26-message-flow-tracing.ko.md#41-실행-중에-기록-수준-변경` anchor 참조(새 문서에서는 `#5-실행-중-기록-수준-변경과-비용-규칙`로 anchor도 바뀜 — 치환표에 절 번호 변경으로 반영)
- 언어별 guide·e2e·sample·contract-inventory 등 mapping.ko.md §1이 집계한 133개 `.md` 파일과 `.json` 3개(경로 문자열만)
- en 짝 문서(`26-message-flow-tracing.en.md`, `27-flow-correlation.en.md`)를 참조하는 en 파일 전체 — en은 캠페인 마지막 단계에서 함께 작성·이동(campaign README §5)

## 교차 주제 확인 필요

mapping.ko.md §4 S10–S13을 그대로 인계한다. 해당 주제를 재작성할 때 아래 문서가
`06-observability`의 새 문서를 올바르게 링크하는지 재확인한다.

- S10 — `24` §2.2·§5의 automatic fanout 15초 record timeout과 `29-transport-liveness`(channel-transport 주제)의 15초 peer deadline이 같은 상수 계열인지. `03`·`04`에는 해당 내용 없음(원 소유는 `01-runtime-monitoring` 몫)
- S11 — `25` §4 label 표의 `close_reason` 닫힌집합 정의는 `04-session/01-stream-session.ko.md`가 인용하는 stream-connector 스펙 §6.3(다른 spec 트리) 소유. `04-flow-correlation.ko.md`에는 `close_reason` 참조 없음(원 소유는 `02-runtime-metrics` 몫)
- S12 — `SafeToShutdown` 판정 조건은 `05-location-relocation`(28·30) 소유. `03`·`04` 본문에는 이 값 자체가 등장하지 않음(원 소유는 `01-runtime-monitoring` 몫)
- S13 — `25` §5의 relocation reason·outcome 식별자는 `30-host-relocation-flow`(location-relocation 주제) 소유. `03`·`04`에는 해당 없음(원 소유는 `02-runtime-metrics` 몫). R92도 같은 교차 확인 대상
- 신규 — `README.ko.md`가 `../04-session/01-stream-session.ko.md`(session 주제, 이미 이동됨)와 `../30-host-relocation-flow.ko.md`(아직 미이동, location-relocation 주제)를 함께 링크한다. `05-location-relocation` 재작성이 끝나 `30`이 옮겨지면 이 README의 링크도 새 경로로 갱신해야 한다
- 신규 — `04-flow-correlation.ko.md` §1이 `../04-message-model.ko.md`를 링크한다. `04-message-model`은 아직 어느 주제로도 재분류되지 않았으므로(topic-map.ko.md 확인 필요) 옮겨질 때 이 링크도 갱신 대상이다

## spec-gap 후보

mapping.ko.md §5 "네 문서의 검증 요구 절" 아래와 §"spec-gap 후보"의 G1–G3 중, 이번
03/04 재작성이 직접 관련된 것만 다시 적는다(전체 G1–G3은 mapping.ko.md와
[spec-gap 대장](../../spec-gap.ko.md)이 단일 출처).

| # | 내용 | 출처 |
|---|---|---|
| G3 | 옛 최상위 `README.ko.md` "디버깅 원칙" §3(R97)이 `message_flow_outcome`의 `error`, `errorType`, `errorMessage`라는 field 이름을 지시하지만 이 이름은 `26-message-flow-tracing`(새 `03-message-flow-tracing`)의 attribute 표에도 `zlink flow:` key 목록에도 없다. 이번 재작성에서는 이 세 이름을 새로 만들지 않고 `03`이 실제로 정의하는 `outcome=failed`/`reason`/error 설명 문자열로 다시 썼다(`README.ko.md#4-간헐-실패를-쫓는-순서` §4.3). 근본 원인(README가 26이 소유하지 않는 이름을 적어 둔 것인지, 26 쪽에 세 field 정의가 빠진 것인지)은 여전히 미해결이며 spec-gap 대장 판정이 필요하다 | 옛 `README.ko.md:250-252`, `26-message-flow-tracing.ko.md:143-177`(§3.2) → 새 `06-observability/README.ko.md` §4.3, `03-message-flow-tracing.ko.md` §3.2 |

G1·G2는 `24`/`25`(새 `01-runtime-monitoring`/`02-runtime-metrics`) 본문에 관한
것이라 이번 03/04 산출물에는 재현하지 않는다 — mapping.ko.md §"spec-gap 후보"가
단일 출처다.

## 완료 확인

- `grep -n ' $'` — `03-message-flow-tracing.ko.md`, `04-flow-correlation.ko.md`,
  `README.ko.md` 모두 결과 없음(clean)
- `grep -c "^\*\*결정"` — `03`·`04` 모두 0
- `grep -c "소급"` — `03`·`04` 모두 0
- README의 anchor 9종(`01-runtime-monitoring.ko.md` 6종, `02-runtime-metrics.ko.md`
  3종, `03-message-flow-tracing.ko.md` 2종, `04-flow-correlation.ko.md` 3종) 전부
  `check_doc_links.py`가 쓰는 `pymdownx.slugs`(`case=lower, unicode=True`)로 재계산해
  대조 — `01`·`02`가 이 작업 도중 병렬로 작성 완료되어 실제 파일 기준으로 전부 OK
  확인(누락 0)
- 파일 줄 수: `03-message-flow-tracing.ko.md` 345줄, `04-flow-correlation.ko.md` 227줄,
  `README.ko.md` 111줄
- 배치 못한 R# — 없음(R91·R92는 이 주제의 다른 문서 담당이므로 이 대장에서는 위치만
  명시하고 배치는 하지 않음)
