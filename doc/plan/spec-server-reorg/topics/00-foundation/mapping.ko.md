# spec/server 재구성 — 00-foundation 매핑표

> 캠페인: `framework/doc/framework/common/spec/server`를 주제별로 재구성하고
> [스펙 문서 작성 가이드](../../../../principal/documentation/spec-writing-guide.ko.md)대로 다시 쓴다.
> 이 문서는 `00-foundation` 주제의 작업 계획이다. 양식은 파일럿
> [`04-session/mapping.ko.md`](../04-session/mapping.ko.md)를 따른다.

관련: [spec-gap 대장](../../spec-gap.ko.md) · [주제 구분](../../topic-map.ko.md) ·
[전체 목차 초안](../../target-readme.ko.md) · [캠페인 지침](../../README.ko.md)

## 1. 대상과 규모

| 현재 문서 | 줄 수 | 성격 | 외부 anchor 링크 수 |
|---|---:|---|---:|
| `00-public-contract-governance` | 197 | 계약 — 공개 계약 소유권과 절차 | 4 |
| `01-glossary` | 2,232 | 계약 — 공통 domain term 정의 | 912 (91개 문서) |
| `02-overview` | 112 | 계약 — Framework 상위 모델 | 0 |
| `03-interaction-model` | 408 | 계약 — operation 대상·완료 의미 | 16 (8개 문서) |
| `04-message-model` | 177 | 계약 — typed 메시지·metadata·codec 계약 | 2 |
| `06-framework-api` | 871 | 계약 — 언어 중립 public API와 등록 규칙 | 44 (37개 문서) |
| `32-framework-error-model` | 146 | 계약 — 공통 `ErrorKind` | 1 |
| `40-internal-layering` | 402 | 구현 스펙 — runtime 책임 경계와 식별자 분리 | 1 |
| 합계 | 4,545 | | |

외부 anchor 링크 수는 `<파일>.ko.md#anchor` 형태로 이 문서를 참조하는 다른 스펙·가이드·e2e
문서의 개수(파일 수)와 anchor 인스턴스 수를 `grep`으로 센 값이다. `01-glossary`는 스펙 전체의
용어 정의 기준이므로 참조가 압도적으로 많다 — 이 주제 안에서 링크·anchor 영향이 가장 큰
문서다(§6).

### 코드가 경로로 여는 곳

- **`06-framework-api.ko.md`** — 세 곳이 경로로 열어 문장을 needle로 검색한다.
  - `framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_layout_contract.cpp`의
    `framework_api_documents_actor_destroy_lifecycle`가 §12(Object Server factory
    선언, `PreserveStateWith` adapter 지정, exact `ActorRef` destroy 오류 표 행)에서 문장 4개를
    needle로 확인한다. 같은 파일의 `session_actor_dispatch_documents_disconnect_destroy_boundary`는
    `20-session-actor-dispatch.ko.md`를 열므로(session 주제 소관) 이 주제와는 별개다.
  - `scripts/verify-framework-submit-api.sh`(`--contract` 모드)가 §7(Logical Multicast 완료)의
    `one-way send·publish는 결과값 없이 정상 완료`, `Target별 수락·실패 결과는 public publish 결과로
    반환하거나 publish 전용 monitoring 값으로 집계하지`, `` `ShuttingDown` `` 세 문장을 확인한다.
  - `scripts/verify-framework-instance-spot-contracts.sh`가 §12에서 문장 9개(actor-free Instance
    Spot factory, Instance Spot manager create 미제공, cold activation 시작 조건, owner claim
    미생성, durable activation inbox 첫 record 확정, `exact SpotRef close` 표 행 등)를 needle로
    확인한다.
- **그 외 7개 문서**는 소스 코드가 경로로 열지 않는다. Java·Node·C++·`.NET` 소스에서 나온
  `01-glossary`·`32-framework-error-model` 매치는 전부 **주석 안의 문서 참조**(`// 01-glossary
  "ObjectGeneration"`, `// Spec 32-framework-error-model:99-103` 같은 줄 번호 인용)이며 경로를
  열어 문장을 검색하지 않는다. 재구성 뒤 주석의 문서 이름·줄 번호가 stale해지지만 contract
  test가 아니므로 §6에서 별도로 갱신 필요 여부만 표시한다.

## 2. 독자 질문 — 주제 README가 답할 것

가이드 §1 질문표를 foundation 주제에 맞게 채운 것. 새 문서의 절은 이 질문 순서를 따른다.

| 질문 | 답이 있어야 할 자리 |
|---|---|
| 이 스펙 문서들은 왜 이렇게 나뉘어 있고, 계약과 구현은 어떻게 구분하는가 | README 개요 + `public-contract-governance` |
| 새 public 계약을 추가하거나 바꾸려면 어떤 절차를 거치는가 | `public-contract-governance` §4 |
| 이 스펙 전체에서 반복해서 나오는 용어(Spot, Actor, owner, generation …)는 정확히 무엇을 뜻하는가 | `glossary` |
| Framework는 무엇을 하는 계층이고, 언어마다 무엇을 독립적으로 구현하는가 | `overview` |
| MeshName·ChannelName·RouteMesh는 각각 무엇을 가리키는가 | `overview` §2 |
| 메시지를 보낼 때 대상은 어떻게 정해지고, 언제 "완료됐다"고 보는가 | `interaction-model` §2 |
| send와 request는 무엇이 다르고, 각각 어떤 조건에서 실패하는가 | `interaction-model` §3~4 |
| Logical Multicast·classic fanout은 서로 무엇이 다른가 | `overview` §4 + `interaction-model` §5~6 |
| 보낸 메시지의 형태(typed payload, metadata, reply)는 어떤 규칙을 따르는가 | `message-model` |
| `ActorRef`·`SpotRef`의 JSON 표현과 codec 규칙은 무엇인가 | `message-model` §2.2~2.3 |
| Application host는 root에 무엇을 등록해야 Framework가 시작되는가 | `framework-api` §2~3 |
| Handler는 어떤 key로 등록되고 filter는 언제 적용되는가 | `framework-api` §8 |
| Send·Request가 실패하면 Application은 어떤 공통 오류를 받는가 | `framework-error-model` |
| `CapacityExceeded`와 `Unavailable`은 어떻게 구분하는가 | `framework-error-model` §5 |
| runtime 코드는 어떤 덩어리로 나뉘고, 어떤 값을 하나로 합치면 안 되는가 | `layering` |
| startup에서 검증하는 것과 runtime에 검증하는 것은 어떻게 다른가 | `framework-api` §14 + `layering` §5 |

## 3. 새 구조

`target-readme.ko.md`가 이미 이 주제의 파일 이름과 번호를 확정해 두었으므로 그대로 따른다
(옛 전역 번호는 파일명에 남기지 않고 목차에 `(옛 NN)`으로만 적는다).

```
spec/server/00-foundation/
  README.ko.md                         주제 진입 1장
  01-public-contract-governance.ko.md  (옛 00)
  02-glossary.ko.md                    (옛 01)
  03-overview.ko.md                    (옛 02)
  04-interaction-model.ko.md           (옛 03)
  05-message-model.ko.md               (옛 04)
  06-framework-api.ko.md               (옛 06)
  07-framework-error-model.ko.md       (옛 32)
  08-layering.ko.md                    (옛 40, 구현 스펙)
```

### 3.0 glossary 처리 방침 (가이드 §3.2)

**핵심 판정: 2,232줄 중 상당량이 "정의"가 아니라 "설명"이다.** 가이드 §3.2는 용어집이
정의의 단일 기준이고, 개별 스펙은 첫 사용 자리에서 그 문맥에 맞는 **한 문장**으로 소개한 뒤
링크만 걸라고 규정한다. 그런데 `01-glossary.ko.md`의 여러 항목은 한 문장 정의를 넘어
다음을 포함한다.

- 정상 흐름 서술 (예: `## 2. Instance Spot 준비`의 cold activation 절 — reservation → factory →
  durable inbox → barrier 순서를 여러 문단에 걸쳐 설명. 이 절차는 이미 `06-framework-api.ko.md`
  §12와 `12-spot-messaging.ko.md`(spot-actor 주제)가 계약으로 소유한다)
- 프로토콜 그림·sequence 설명 (`## 10. STREAM session과 Actor binding`의 여러 항목)
- 다른 문서가 이미 표로 갖고 있는 오류 조건 재나열

**제안하는 분리 기준** — 항목별로 다음 세 갈래로 나눈다.

| 갈래 | 판정 기준 | 처리 |
|---|---|---|
| A. 순수 정의 | 형태·공개 구성·생성/관리·수명 요약 표 + 정의 문장 1~3개로 끝남 | 그대로 유지 |
| B. 정의 + 소유 문서의 절차 서술 | 정의 뒤에 특정 계약 문서(예: `interaction-model`, `framework-api`,
  `12-spot-messaging`)가 이미 소유한 흐름·표·오류 조건을 다시 풀어 씀 | 정의 표만 남기고 절차
  문단은 삭제, 대신 "자세한 절차는 [X](경로#절)가 소유한다" 링크 한 줄로 대체 |
| C. 소유 문서가 아직 없는 절차 (해당 문서가 이 주제 밖 — 예: relocation·session 주제 문서) | B와 같은
  기준이나 소유 문서가 다른 주제라 아직 재작성 전임 | 정의 표만 남기고, 링크는 **옛 경로**로
  걸어 두었다가 해당 주제가 끝나면 새 경로로 치환(§6과 동일한 후속 처리) |

이 기준으로 재작성 에이전트가 항목마다 A/B/C를 판정하고 표에 기록한다(§5 R-glossary 참조).
목표는 줄 수를 임의로 줄이는 것이 아니라 **같은 절차 설명이 두 곳에 있는 상태를 없애는 것**이다.
정의 자체(형태·공개 구성·생성/관리·수명)는 압축하지 않는다 — 이것이 용어집의 유일한 존재
이유다.

Glossary 구조(11개 절, `## N. 주제`, 각 항목 `### 용어` + 요약 표)는 가이드가 이미 승인한
형식이므로 유지한다. `.NET` pseudocode 블록도 "공개 구성"을 정확히 읽기 위한 보조 표기이므로
유지한다.

### 3.1 `public-contract-governance` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. 공개 계약이란 무엇인가 | 00 §1 | 계약 |
| 2. 계약 소유권 (공통 스펙 / 언어별 exact interface / Core / internals) | 00 §2, §2 결합 규칙 문단 | 계약 |
| 3. Production source owner | 00 §2.1 | 계약 |
| 4. 새 계약에 반드시 고정할 항목 | 00 §3 | 계약 |
| 5. 공개 계약 절차 (7단계) | 00 §4 | 계약 |
| 6. 언어별 표현 원칙 | 00 §5 | 계약 |
| 7. 설계 검토 기준 | 00 §6, §6.1 | 계약 |
| 8. 검증 | 00 §7 | 검증 |
| (부록 또는 별도 관리 문서로 이관) 11.0 spec-first 이관 정책 | 00 §8 | → §4 S4 참고, 이번 재작성 범위 밖 |

### 3.2 `overview` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. 한 줄 정의 — Framework가 하는 일과 언어별 독립 구현 경계 | 02 §1 | 계약 |
| 2. MeshName·ChannelName·RouteMesh | 02 §2 | 계약 |
| 3. 메시지 대상 선택 | 02 §3 | 계약 |
| 4. Logical Multicast와 classic fanout | 02 §4 | 계약 |
| 5. 실행 owner (Node/Spot/Actor/STREAM session) | 02 §5 | 계약 |
| 6. 연결 관리 (automatic discovery, manual peer) | 02 §6 | 계약 |
| 7. Framework가 숨기는 것 | 02 §7 | 계약 |

### 3.3 `interaction-model` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. 공통 모델 — 대상 선택과 완료 표 | 03 §2 | 계약 |
| 2. 상호작용을 시작하는 public interface | 03 §2.1 | 계약 + 선언(코드블록은 유지, 옵션 표는 없음) |
| 3. Node direct와 channel select-one | 03 §3 | 계약 |
| 4. Send와 request | 03 §4 | 계약 |
| 5. Spot Logical Multicast | 03 §5 | 계약 |
| 6. Classic fanout | 03 §6 | 계약 |
| 7. Spot과 Actor | 03 §7 | 계약 |
| 8. STREAM session | 03 §8 | 계약 |
| 9. 대표 public 호출 예제 | 03 §9, §9.1~9.4 | 선언(예제 코드) |
| 10. Handler 실패 | 03 §10 | 계약 |
| 11. 종료 (Relocate/Shutdown이 상호작용에 미치는 영향) | 03 §11 | 계약 |

### 3.4 `message-model` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Typed 메시지 | 04 §1 | 계약 |
| 2. 메시지 종류와 완료 | 04 §2 | 계약 |
| 3. MessageContext | 04 §2.1 | 계약 |
| 4. Global object reference JSON (`ActorRef`/`SpotRef`) | 04 §2.2 | 계약 |
| 5. `framework-json-v1` typed payload profile | 04 §2.3 | 계약 |
| 6. Application metadata | 04 §3 | 계약 |
| 7. 전달 규칙 | 04 §4 | 계약 |
| 8. Ownership과 크기 제한 | 04 §5 | 계약 |

### 3.5 `framework-api` 절 구성

871줄로 가장 크다. 절 순서는 유지하되 §2.1의 옵션 표를 인라인 주석으로 바꾸고(§4 S6),
Spot·Actor·STREAM owner(§12)를 질문 단위로 나눈다.

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Public contract와 runtime implementation의 경계 | 06 §1, §1.1 | 계약 |
| 2. Root 등록 | 06 §2 | 계약 |
| 3. Core memory budget과 Application job queue 설정 | 06 §2.1 | 계약(옵션은 선언 블록의 인라인 주석으로) |
| 4. RouteMesh 등록 | 06 §3 | 계약 |
| 5. Manual peer | 06 §4 | 계약 |
| 6. 메시징 API family | 06 §5 | 계약 |
| 7. Call operation | 06 §6 | 계약 |
| 8. Logical Multicast 완료 | 06 §7 | 계약 (cpp/verify-submit-api needle 원문 유지) |
| 9. Handler 등록과 dispatch | 06 §8 | 계약 |
| 10. Handler filter | 06 §8.1 | 계약 |
| 11. Handler 실행 객체와 dependency 수명 | 06 §8.2 | 계약 |
| 12. Codec | 06 §9 | 계약 |
| 13. Location Store와 Relocation Store 등록 | 06 §10 | 계약 |
| 14. Classic fanout 등록 | 06 §11 | 계약 |
| 15. User·Instance Spot과 Actor factory 등록 | 06 §12 앞부분(Spot type, Entry Spot ID, factory) | 계약 (verify-instance-spot-contracts needle 원문 유지) |
| 16. Missing object 생성 — cold activation 순서 | 06 §12 activation envelope 문단 | 계약 |
| 17. Create·GetOrCreate 결과와 relocation policy | 06 §12 나머지 | 계약 |
| 18. `Yield`와 STREAM/Actor 등록 마무리 | 06 §12 말미 | 계약 |
| 19. 오류 kind (링크) | 06 §13 | 계약(요약 + 링크) |
| 20. Operation 결과 변환 | 06 §13.1 | 계약 |
| 21. Dispatch 실패 action owner | 06 §13.2 | 계약(링크) |
| 22. Startup validation | 06 §14 | 계약 |
| 23. Runtime query와 monitoring | 06 §15 | 계약 |

### 3.6 `framework-error-model` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. 범위 | 32 §1 | 계약 |
| 2. 공통 `ErrorKind` (표 13개 값) | 32 §2 | 계약 |
| 3. 호출 전에 확인할 수 있는 오류 | 32 §3 | 계약 |
| 4. `Send` 완료와 실패 | 32 §4 | 계약 |
| 5. `Request` 완료와 실패, `CapacityExceeded` vs `Unavailable` | 32 §5 | 계약 |
| 6. Typed 결과와 `Rejected` | 32 §6 | 계약 |
| 7. 재시도 판단 | 32 §7 | 계약 |
| 8. 검증 | 32 §8 | 검증 |
| 9. Application job queue 포화 | 32 §9 | 계약 |

### 3.7 `layering` 절 구성 (구현 스펙 — 문서 성격 유지)

`**결정.**`/`**결정 —**` 라벨 13곳을 제거하고(§4 S2) 굵은 규칙 문장 + 이유 불릿으로 바꾼다.
`정본` 5회를 "이 규칙을 소유하는 문서"/"단일 기준"으로 바꾼다(§4 S3). 절 순서 자체는 유지한다.

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. binding 경계를 의미 기준으로 잡는다 | 40 §1 | 구현 결정(라벨 제거) |
| 2. 종료를 topology마다 두지 않는다 | 40 §2 | 구현 결정(라벨 제거) |
| 3. 종료에는 두 가지 의도가 있다 | 40 §3 | 구현 결정(라벨 제거) |
| 4. 정리 순서를 고정한다 | 40 §4 | 구현 결정(라벨 제거) |
| 5. 등록 선언은 시작할 때 한 번만 검증한다 | 40 §5 | 구현 결정(라벨 제거) |
| 6. 식별자를 합치지 않는다 | 40 §6 | 구현 결정(라벨 제거) |
| 7. 확인할 결과 | 40 §7 | 검증 |

## 4. 읽으면서 발견한 구조 문제 (재작성에서 처리)

| # | 문제 | 처리 |
|---|---|---|
| S1 | `01-glossary`가 최소 3개 항목(cold activation 절차, STREAM/Actor binding 절차, 여러 오류 조건 표)에서
  다른 계약 문서가 이미 소유한 절차를 다시 풀어 쓴다. 2,232줄 중 상당 비중이 정의가 아니라 절차 재서술 | §3.0의 A/B/C 판정으로 분리. B/C 항목은 정의 표만 남기고 절차 문단을 링크 한 줄로 대체 |
| S2 | `40-internal-layering`이 `**결정.**`/`**결정 —**` 라벨을 13곳에서 사용한다(`grep -c "^\*\*결정" 40-internal-layering.ko.md` = 13). 가이드(§2.4, §6)는
  "결정" 같은 라벨을 금지하고 굵은 규칙 문장 + 이유 불릿으로 쓰라고 명시한다 — session 파일럿에서
  나온 형식 피드백이 이미 가이드에 반영되어 있는데 40이 옛 형식 그대로 남아 있다 | 라벨 제거, 굵은
  규칙 문장(예: "resource는 만든 쪽이 닫는다") + 이유 불릿으로 재작성. mermaid 그림은 유지 |
| S3 | "정본"(널리 안 쓰는 한자어) 5회 — `00-public-contract-governance` §2 소제목("공개 계약 정본과
  internals의 결합 규칙") 1회 + §8 제목·본문 2회(총 3회), `40-internal-layering` §1·§2 각 1회(총 2회) |
  "이 규칙을 소유하는 문서" 또는 "단일 기준"으로 교체(원칙 7.3) |
| S4 | `00-public-contract-governance` §8 "11.0 spec-first 정본 규칙"은 특정 마이그레이션(Core service
  이관, 11.0)의 일회성 전환 정책이며, 이 문서의 나머지 절(영구적인 계약 소유권 규칙)과 성격이 다르다.
  가이드는 "Internals는 migration 이력이나 진행표를 포함하지 않는다"고 하는데, 이 절은 계약
  문서에 마이그레이션 서술이 섞인 반대 사례다 | 이번 재작성에서는 계약 문서 성격 유지가 원칙이므로
  절 자체를 삭제하지 않고 그대로 옮기되, 문서 끝(부록)으로 이동해 본문 계약 절차와 분리한다.
  실제로 마이그레이션이 끝났는지, 다른 위치(실행 ledger)로 옮겨야 하는지는 spec-gap 후보로만 기록(§ spec-gap) — 내용 판단은 이번 캠페인 범위 밖 |
| S5 | `03-interaction-model` §2와 `04-message-model` §2가 거의 같은 "메시지 종류 → 대상 선택 → 완료" 표를
  중복 서술한다(operation family 나열, send/request 완료 조건) | `message-model` §2는 message *종류*(Send/Request/Logical
  Multicast/fanout/STREAM)와 완료 조건만 소유, `interaction-model` §1은 *operation*(node direct/channel/Spot/Actor/…)의
  대상 선택 방식을 소유하도록 관점을 분리해 서술을 좁힌다. 겹치는 완료 문장은 `interaction-model`에서
  `message-model`을 링크 |
| S6 | `06-framework-api` §2.1에 옵션 설명 표가 4개(`설정 \| 의미` ×2, `Profile \| Jobs per effective
  processor`, `설정 \| 기본값 \| 적용 범위와 의미`) — 가이드 §8.3은 "선언의 옵션 설명은 표가 아니라
  인라인 주석으로" 규정한다 | 각 표를 builder/option 선언 pseudocode 블록 + 옵션마다 인라인 주석으로
  바꾼다. 숫자·기본값·범위는 그대로 보존(값 손실 없음) |
| S7 | `06-framework-api` §12(Spot, Actor와 STREAM owner)가 120줄 문단 벽 — factory 등록, Entry Spot ID
  발급, cold activation 절차, Create/GetOrCreate 결과, relocation policy, `Yield`, STREAM 등록이
  한 절에 이어져 있다 | §3.5 표대로 §15~18 네 절로 분리(질문 기준: "무엇을 등록하는가" → "없는
  객체를 어떻게 만드는가" → "생성 결과는 어떻게 갈리는가" → "그 외 등록") |
| S8 | `06-framework-api` §1 제목 "1. 목적"이 메타 제목(가이드 §2.2가 금지) | 문서 내용을 가리키는 제목으로 교체(예: "Public 계약과 runtime의 경계") |
| S9 | `32-framework-error-model` §5에 **굵게**로 강조된 규칙 설명 불릿(`CapacityExceeded`/`Unavailable`
  구분)이 이미 굵은 규칙 문장 형식과 가깝다 — 형식 위반은 아니지만 리스트 안에 "반면", "이 구분은
  대기열에만 적용한다" 같은 대화체 접속사가 섞여 있다 | 재작성 시 각 불릿을 독립된 규칙 문장으로
  다듬고 "반면" 같은 구어체 연결을 제거 |
| S10 | `01-glossary` "표와 .NET 코드 예제를 읽는 방법" 절이 글머리에서 사실상 용어 읽는 법을 설명하는
  메타 절 — 다른 문서라면 §2.2 위반이지만 용어집은 항목이 아니라 "이 문서를 읽는 방법"이므로 예외로
  본다(세션 파일럿에도 없던 문서 종류) | 유지. 다만 위치를 README 링크 다음, 첫 `## 1.` 절 앞으로
  고정해 다른 절과 섞이지 않게 한다(이미 그 위치) |
| S11 | `06-framework-api` §9(Codec)의 "언어 | server root 등록 | Stream Connector 등록 | exact interface
  owner" 표는 언어별 정확한 심볼을 나열하지만 이 표 자체가 각 언어 exact interface의 축약 복제에
  가깝다 | 유지하되 "이 표는 요약이며 실제 심볼은 owner가 최종 기준" 문구를 표 앞에 명시(가이드
  §3.3 patttern) |

## 5. 규칙 등가성 대장 — 초기 추출

재작성 뒤 이 표의 모든 행이 새 문서의 정확히 한 절에 있어야 한다("새 위치" 열은 재작성 뒤 채움).
행이 없으면 누락, 표에 없는 보장이 새 문서에 있으면 추가 보장(둘 다 대조 실패). Glossary는
§3.0 방침에 따라 항목별 정의 자체를 R로 뽑지 않고, 구조 규칙만 R-G로 뽑는다.

### `public-contract-governance` (R1~R22)

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| R1 | 공개 계약 = 타입·operation + timeout·취소·오류·callback 순서·ownership·완료 조건 | 00 §1 | |
| R2 | 계약 소유권 4분류(공통 spec / 언어별 `languages/<lang>/` / Core spec / Framework internals)와 각 소유 범위 | 00 §2 | |
| R3 | 공통 스펙은 특정 언어 문법을 표준으로 삼지 않는다 | 00 §2 | |
| R4 | 공개 동작은 공통 spec + 언어별 exact interface에 **한 번만** 정의; internals는 링크 후 구조만 설명 | 00 §2 결합 규칙 | |
| R5 | 충돌 시 공개 동작·public API는 spec이 우선; internals가 public API·오류 의미·failover 범위를 새로 만들 수 없음 | 00 §2 결합 규칙 | |
| R6 | 배포 package마다 production source owner 하나만 존재 | 00 §2.1 | |
| R7 | Application/Server/HTTP client/Stream Connector는 각각 독립 package, 자기 contract owner를 둠 | 00 §2.1 | |
| R8 | public constructor·factory·builder·free function·extension·DTO·value·enum·error도 interface와 같은 source가 소유 | 00 §2.1 | |
| R9 | Runtime 구현은 contract 참조, contract source는 runtime 참조 안 함(단방향) | 00 §2.1 | |
| R10 | `runtime/internal` declaration은 외부 import 불가; 이름만 internal로 바꾸는 것으로는 불충분 | 00 §2.1 | |
| R11 | 외부 provider SPI는 최소 abstraction artifact가 소유 가능하나 전체 contract를 SPI로 옮기지 않음 | 00 §2.1 | |
| R12 | package-neutral artifact는 여러 package가 공유해야 하는 최소 contract(codec·error type identity)에만 | 00 §2.1 | |
| R13 | Namespace/FQN은 exact interface가 정함; 정리 목적으로 바꾸지 않음; layout 변경은 snapshot·consumer build·owner gate 통과 필요 | 00 §2.1 | |
| R14 | 새 공통 계약에 고정할 8항목(package·owner, 입력·결과·호출 시점, timeout/cancel/backpressure, callback 순서, ownership, 오류 구분, discovery/manual 기준, contract test/E2E 결과) | 00 §3 | |
| R15 | 공개 계약 절차 7단계(공통 기록→언어별 기록→전환 inventory→Core/bindings 정합→검증→export 대조→독립 리뷰) | 00 §4 | |
| R16 | 공통 E2E·다른 언어 코드는 해석 검증 자료일 뿐 public interface 근거가 아님 | 00 §4 | |
| R17 | 언어별 관례 5개(.NET Task/ValueTask/CancellationToken/DI, Java CompletionStage, Kotlin suspend/Flow, Node Promise/AbortSignal, C++ 명시적 ownership/coroutine) | 00 §5 | |
| R18 | 한 언어 타입 이름·overload를 다른 언어에 복제하지 않음; 기능·완료 조건·오류 의미가 같으면 같은 계약 투영 | 00 §5 | |
| R19 | 설계 검토 기준 6항목(select+submit 단일 operation, endpoint/peer/encoding/correlation은 runtime 소유, typed handle/context가 주소·generation 보존, 이름 중복 금지, boolean·nullable 조합 금지, operation별 설정은 call object에만) | 00 §6 | |
| R20 | 언어별 exact interface는 application 직접 사용 API + 필수 SPI만 기록; runtime 내부 배선·저장 row·key·command·watch·publisher·dispatcher invocation은 비공개; 세부 capability interface로 쪼개 노출 금지; 공개 declaration이 0개면 문서 삭제 | 00 §6.1 | |
| R21 | contract test 검증 9항목(export, contract source 밖 declaration, 역방향 dependency, visibility 차단, 시그니처, generic/nullable/optional/기본값/overload, 비동기·timeout·취소, 공개 오류 kind·lifecycle callback, MeshName/ChannelName/RID/owner 계약, Redis store 등록) | 00 §7 | |
| R22 | 검증은 source tree만이 아니라 실제 배포 package를 외부 consumer가 참조한 결과로 확인 | 00 §7 | |

### `glossary` — 구조 규칙만 (RG1~RG8)

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| RG1 | 값·record 용어는 먼저 요약 표(형태/.NET 표기/공개 구성/생성·관리/수명)를 둔 뒤 필요하면 실제 `.NET` 선언 | 01 "표와 코드 예제를 읽는 방법" | |
| RG2 | `.NET 표기`에 "public type 없음"이면 코드 블록은 contract pseudocode이며 실제 API 이름이 아님 | 01 서두 | |
| RG3 | 실제 `.NET` 선언의 단일 기준은 `.NET` exact interface 문서; 용어집 표기는 보조 | 01 서두 | |
| RG4 | 용어집은 11개 주제 절(`## N.`)로 나뉘고 각 항목은 `<a id>` + `### 용어` 고정 형식 | 01 목차 구조 | |
| RG5 | 용어는 같은 이름을 다른 개념에 재사용하지 않음(유일성) — 예: `OperationId`가 두 계약에서 쓰이면 문서 안에서 구분해 명시 | 01 각처 | |
| RG6 | 용어집은 여러 스펙이 공유하는 정의의 기준이며, 개별 스펙은 첫 사용 자리에서 한 문장 + 링크로만 소개(가이드 §3.2, 이 문서가 준수해야 할 대상 규칙) | 가이드 §3.2 | |
| RG7 | 항목이 다른 계약 문서가 소유한 절차를 다시 서술하면 안 됨(§3.0 A/B/C 판정 적용 대상) | §3.0 | |
| RG8 | 새 용어 추가 조건 4가지(가이드 §3.4 인용 — 기존 identifier로 설명 불가, 여러 문서 반복 사용, 이름이 반복 설명보다 이해가 쉬움, 다른 용어와 안 겹침)를 용어집 서문에 명시할지 판단 | (신규 — 가이드 §3.4를 옮길지 여부) | |

### `overview` (R23~R39)

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| R23 | Framework 한 줄 정의(handler·RouteMesh·Channel·Spot·Actor·STREAM·fanout·location을 host lifecycle/DI에 연결) | 02 §1 | |
| R24 | 5언어 각자 service runtime 구현; binding public raw socket API만 사용; 공통 native runtime·service C ABI 없음; Java/Kotlin은 JVM runtime 공유 | 02 §1 | |
| R25 | MeshNode 1개 = routing ID 1 + ROUTER endpoint 1 | 02 §2 | |
| R26 | MeshName(물리 mesh) vs ChannelName(process-local 논리 주소) 역할 구분 | 02 §2 | |
| R27 | 한 process가 여러 MeshName의 MeshNode를 가질 수 있음; mesh는 독립적, 자동 relay 없음 | 02 §2 | |
| R28 | 같은 process의 ChannelName 1개는 RouteMesh 또는 ClientServer topology 1개에만 대응 | 02 §2 | |
| R29 | 대상 선택 5분류(node direct/channel select-one/Logical Multicast/Spot·Actor global ID/create·get-or-create) | 02 §3 | |
| R30 | 선택과 submit은 하나의 operation(별도 send 반복 없음) | 02 §3 | |
| R31 | Logical Multicast: remote MeshNode마다 1회 routed message + local subscription 검사; 같은 node 여러 Spot 일치 시 storage reference 공유 | 02 §4 | |
| R32 | Logical Multicast remote 송신은 ROUTER HWM·timeout·backpressure 규칙 따름; 뒤 target 실패가 앞 target 제출을 취소하지 않음 | 02 §4 | |
| R33 | classic fanout: 연결+구독 준비된 subscriber에만 전달; automatic publisher는 descriptor 게시, subscriber는 live publisher 전부 연결; manual endpoint는 store 없이 구성 가능; 저장·replay 보장 없음 | 02 §4 | |
| R34 | 실행 owner 4분류(Node/Spot/Actor/STREAM session)와 각 책임; Instance Spot은 direct·timer만, Actor membership·Logical Multicast subscription 없음 | 02 §5 | |
| R35 | Spot·Actor message를 Node handler가 재분배하도록 요구하지 않음; transport readiness·service protocol frame은 callback에 비노출 | 02 §5 | |
| R36 | automatic discovery는 location store descriptor+lease 사용; RouteMesh는 MeshName descriptor, ClientServer client는 ChannelName server descriptor(서로 대체 불가) | 02 §6 | |
| R37 | manual peer도 automatic peer와 동일한 MeshName/RID/ChannelName/generation/security admission 통과 | 02 §6 | |
| R38 | Framework가 숨기는 것(transport 주소 선택, peer reconnect, multipart framing, packet codec, reply correlation, backpressure queue) | 02 §7 | |
| R39 | 외부 edge gateway 인증·quota·WAF·API versioning·billing은 계약 범위 밖 | 02 §7 | |

### `interaction-model` (R40~R80)

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| R40 | 공통 모델 9행 표(대상 선택·완료 — node direct send/request, channel send/request, Logical Multicast, Spot/Actor message, create/get-or-create, classic fanout, STREAM) | 03 §2 | |
| R41 | select-one 정의(Framework가 조건에 맞는 target 하나를 고름) | 03 §2 | |
| R42 | 상호작용 시작 interface 8행 표(`IZLinkRouteClient` 등) | 03 §2.1 | |
| R43 | `Send...Async()`는 admission까지, `Request...Async<TReply>()`는 reply까지; `Yield<TReply>()`는 `SpotWide`/Instance Spot shared turn에서만 | 03 §2 말미 | |
| R44 | Node direct: 대상이 Mesh member 아니면 `NotFound`, member인데 미준비면 대기 후 `Unavailable`; 자동 재전송 없음 | 03 §3 | |
| R45 | Global Spot·Actor message는 cached Ready route·committed Message Follow route만 사용; 실패 시 `Unavailable`, 다른 owner에 자동 재제출 없음 | 03 §3 | |
| R46 | Channel: process-local 경로 결정 → RouteMesh는 weight>0 ready member, ClientServer는 ready server; 선택·submit 사이 callback 없음 | 03 §3 | |
| R47 | Weight 0은 새 channel 선택·Logical Multicast remote target에서 제외; RID direct·이미 제출한 operation에는 영향 없음 | 03 §3 | |
| R48 | select-one은 첫 binding operation 시작 직전에만 선택; 시작 후 Core가 HWM 재시도·completion 소유, Framework는 재선택·replay 안 함 | 03 §3 | |
| R49 | Node direct는 RID, Spot·Actor는 global ID, session은 binding token 유지; 물리 peer lifecycle generation은 비공개 | 03 §3 | |
| R50 | 같은 ChannelName을 여러 물리 경로에 등록 불가; 다른 topology에 등록하면 startup 오류 | 03 §3 | |
| R51 | `send`는 one-way, 비동기 submit만 제공(동기 terminator 없음); 반환은 outbound queue 수락, handler 실행 확인 아님 | 03 §4 | |
| R52 | send timeout까지 admission 대기; 수락 후 오류는 logger/telemetry로만 보고, 전용 error sink 없음 | 03 §4 | |
| R53 | Global Spot·Actor send는 같은 비동기 terminator; cache hit도 같은 공개 의미(동기 제공 안 함) | 03 §4 | |
| R54 | Message call은 기본적으로 creation intent 안 만듦; Instance intent 명시 시에만 cold activation | 03 §4 | |
| R55 | one-way 실패 분류: capacity 미확보 `DeadlineExceeded`, target/route 부재·shutdown은 exception, invalid arg/handle/state·중복 submit은 local exception, cancellation은 cancelled awaitable; 자동 재제출 없음 | 03 §4 | |
| R56 | request는 reply correlation 생성, terminal 결과 정확히 1회; request timeout=reply 대기, send timeout=전송 backpressure; route 오류·timeout 뒤 자동 재전송 없음 | 03 §4 | |
| R57 | Spot origin request는 원래 activation·generation을 completion record에 보존; reply를 새 message로 재dispatch 안 함 | 03 §4 | |
| R58 | 같은 origin→같은 destination pipe는 FIFO; 다른 destination/origin/session 간 전역 순서 미보장 | 03 §4 | |
| R59 | Logical Multicast publish: target ChannelName·topic·payload, publish 시점에 snapshot | 03 §5 | |
| R60 | remote MeshNode마다 1회 submit, `(ChannelName, topic filter)` local subscription 검사, 같은 node 일치 Spot은 reference 공유, relay·replay 없음 | 03 §5 | |
| R61 | bounded I/O executor에 제출; send timeout 안에 시작 못하면 `DeadlineExceeded`; 시작하면 정상 완료(반환값 없음), 이후 target 제출은 내부에서 계속 | 03 §5 | |
| R62 | transaction 시작이 commit point — cancellation/shutdown이 남은 target 제출을 중단 안 함; 앞 target은 뒤 target 실패로 취소 안 됨 | 03 §5 | |
| R63 | snapshot target 0개도 정상 완료; target별 결과는 public 결과·monitoring에 미집계; rollback·retry 없음 | 03 §5 | |
| R64 | 정상 완료 = transaction 시작 의미일 뿐, target 제출·handler 실행·수신 큐 수락을 보장하지 않음 | 03 §5 | |
| R65 | classic fanout: MeshNode 독립 publisher/subscriber; 연결 전·단절 중 저장 없음, 재연결 후 replay 없음 | 03 §6 | |
| R66 | publisher call은 publisher socket send timeout까지 admission 대기하는 비동기 terminator 1개; subscriber 0이어도 정상 완료(수신·handler 완료 아님) | 03 §6 | |
| R67 | publish 공통 입력(ChannelName, topic, typed event); topic 생략 편의 호출은 packet name을 topic으로 사용; subscriber dispatch는 packet name으로 handler 선택 | 03 §6 | |
| R68 | automatic subscriber는 같은 ChannelName의 live publisher 전부 연결, 다른 ChannelName/descriptor kind 제외; manual subscriber는 명시 endpoint만 | 03 §6 | |
| R69 | Spot direct message·Logical Multicast·timer·lifecycle callback은 execution gate에서 직렬; `SpotWide`는 공통 gate, `PerActor`는 Actor별/lane별/timer별 gate; Node callback이 Spot queue 대신 읽지 않음 | 03 §7 | |
| R70 | Instance Spot 생성은 Spot direct fluent call의 명시적 Instance intent만 시작; Location Store reservation owner가 factory 실행 → first record 확정 → recovery root/cursor 포함 `Ready` commit → local queue head 복원 → barrier open; 경쟁자는 합류만 | 03 §7 | |
| R71 | `ActorRef`/`SpotRef` = global ID + ObjectGeneration + 조회 시점 MeshName·NodeRid의 immutable snapshot; endpoint·내부 frame·runtime resource 미포함 | 03 §7 | |
| R72 | Bound session `Ref`/`ref()`는 relocation route switch 완료 시 새 immutable snapshot 반환, 기존 반환값은 불변; 일반 message는 ref 아닌 global ID로 current authority resolve | 03 §7 | |
| R73 | Actor message는 global ID resolve 후 Actor mailbox에 직접 추가(Spot queue 미경유); Entry Spot Actor·`PerActor` Spot Actor는 Actor별 gate, `SpotWide`는 공통 gate | 03 §7 | |
| R74 | Node·Spot·Actor·binding operation completion은 application handler 대기 중에도 진행 가능한 infrastructure 실행 영역에서 처리 | 03 §7 | |
| R75 | STREAM: recv loop가 관리 queue에 넣은 뒤 callback 실행; 같은 session 내 packet·lifecycle callback은 직렬, session 간 전역 순서 미보장 | 03 §8 | |
| R76 | bind된 session ingress는 Actor mailbox로 submit; egress는 현재 binding의 session FIFO 사용; 이동 중 barrier가 old/new epoch 구분 | 03 §8 | |
| R77 | bound session send·relay·명시적 STREAM reply는 async-only one-way admission; STREAM reply는 자기 소켓 send timeout 사용(caller request timeout을 admission deadline으로 안 씀) | 03 §8 | |
| R78 | reply token: 유효한 첫 terminator가 admission 전 원자적으로 소비; backpressure/timeout/cancel로 완료돼도 재사용 안 함; 두 call 경쟁 시 하나만 admission 시작 | 03 §8 | |
| R79 | Handler 실패: 복원 가능한 request는 구조화 error reply, 그 외/one-way는 drop+log+metric; handler 예외도 one-way에서 기록; logger 실패가 원래 결과를 안 바꿈 | 03 §10 | |
| R80 | 종료: `Relocate`/`Shutdown`이 새 channel 선택·Logical Multicast target·새 상태 배정 제한; `Relocating`은 permit 획득 unit부터 진행, 나머지는 큐 turn 경계에서 seal; `Draining` 뒤엔 이미 admission한 것만 deadline까지; Draining node는 새 placement 후보 제외; `Shutdown`은 Instance Spot 미이동, `Relocate`는 policy·transaction이 허용한 owner만 이동; 둘 다 admission seal·current authority 검증 | 03 §11 | |

### `message-model` (R81~R107)

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| R81 | Framework envelope·multipart encoding은 모든 언어 공통 wire schema/fixture, application에 비노출 | 04 서문 | |
| R82 | 기본 typed JSON serializer 사용; 호출자는 codec·registry를 메시지 타입마다 등록하지 않음 | 04 §1 | |
| R83 | codec extension은 host 단위 정책(handler·호출마다 반복 전달 안 함); raw bytes API는 transport 검사·codec 구현 전용 | 04 §1 | |
| R84 | 메시지 종류 5행 표(Send/Request/Logical Multicast/Classic fanout publish/STREAM)와 각 완료 조건 | 04 §2 | |
| R85 | reply correlation은 transport operation ID 또는 session sequence 소유; packet name·metadata는 매칭 key 아님; 성공/오류 중 하나로 1회 완료 | 04 §2 | |
| R86 | Object creation request: 최대 1 MiB encoded content reference+hash를 reservation 전 durable intent에 기록; factory는 at-least-once에도 같은 결과로 수렴; CAS loser는 일반 message로 안 보냄; generation/attempt/lease token은 message payload·context에 미포함 | 04 §2 | |
| R87 | MessageContext 필드(nullable MeshName/ChannelName, PacketName, ContentType, immutable Metadata, UTF-8 nullable CorrelationId); CorrelationId는 send=null, request=non-null | 04 §2.1 | |
| R88 | MeshName은 RouteMesh/Spot·Actor dispatch에서 non-null, ClientServer/STREAM에서 null | 04 §2.1 | |
| R89 | Connection cancellation은 universal context 아닌 언어별 handler 인자/Session 전용 context 소유 | 04 §2.1 | |
| R90 | 경로별 특화 context(`RouteMessageContext`/`PublishMessageContext`/`SessionMessageContext`) | 04 §2.1 | |
| R91 | Send/Request/SpotActor 별도 marker context 없음; Actor request context에 reply metadata/compression option 없음; filter 전용 context는 dispatch 종류 5값만 구분(Spot·Actor·Logical Multicast·STREAM에는 dispatch 종류 없음) | 04 §2.1 | |
| R92 | `ActorRef`/`SpotRef` JSON: 모든 property required, case-sensitive, 중복/`null`/unknown property/범위 밖 generation 거부; 정규화 없음 | 04 §2.2 | |
| R93 | JSON 필드명(`actorId`/`spotId`, `objectGeneration`, `meshName`, `nodeRid`) 정확한 예시 스키마 | 04 §2.2 | |
| R94 | `objectGeneration`은 `"1"`..`"9223372036854775807"` leading-zero 없는 decimal string; 부호·소수점·exponent 불허 | 04 §2.2 | |
| R95 | `framework-json-v1` profile 규칙 11항목(BOM 불허, 대소문자 구분, 순서/whitespace 무의미, 중복/누락 거부, unknown 무시, null은 nullable만, 정수 표현, float finite만, byte는 base64, date/decimal/UUID는 명시 표현) | 04 §2.3 | |
| R96 | 더 엄격한 DTO 계약(예: §2.2)이 profile보다 우선 | 04 §2.3 | |
| R97 | profile 요소 변경은 breaking contract change; relocation adapter opaque state bytes에는 미적용 | 04 §2.3 | |
| R98 | Application metadata 계약 7행 표(key/value UTF-8+NUL 불허, 전체 1024 bytes, 같은 key는 마지막 값, 수신은 불변 snapshot, lifetime은 handler turn까지, malformed는 protocol 오류로 handler 미호출, reply는 자동 복사 없음) | 04 §3 | |
| R99 | metadata 내부 frame 배치·encoding은 비공개; relay 경로에서도 application이 frame 조립 안 함 | 04 §3 | |
| R100 | 전달 규칙 7행 표(Node direct/Channel, Spot, Logical Multicast, Actor, STREAM, bound session↔Actor allowlist) | 04 §4 | |
| R101 | Framework가 새 request 만들 때 원본 metadata 자동 복사 없음; 호출자가 명시적으로 넘긴 경우만 포함 | 04 §4 | |
| R102 | submit 반환 전까지 outbound builder·payload는 호출자 소유; 수락 후 Framework가 lifetime 유지; 호출자에게 transport buffer/native pointer/multipart lifetime 관리 요구 안 함 | 04 §5 | |
| R103 | handler에 전달된 context/metadata/payload view는 callback 동안 읽기 전용; application이 dispose 안 함, 보관하려면 복사 | 04 §5 | |
| R104 | 수락한 message는 typed payload를 최대 1회만 역직렬화; 첫 접근 값/실패를 보관, 재접근 시 codec 재호출 없음; type mismatch 시 언어별 오류; raw view/명시적 byte 복사는 이 typed 결과를 안 만듦 | 04 §5 | |
| R105 | Object creation pending 중에도 같은 ownership 규칙; Framework가 immutable encoded payload를 content store에 고정; Ready/fenced failure 뒤 해당 attempt storage 1회 해제 | 04 §5 | |
| R106 | payload 최대 크기는 대상 transport `MaxMessageSize`; 초과 시 submit/receive 전체 실패 | 04 §5 | |
| R107 | Logical Multicast target별 제출·결과 집계는 `12-spot-messaging`(spot-actor 주제) 소유 — 링크만 | 04 §5 | |

### `framework-api` (R108~R240)

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| R108 | Public contract/domain contract에 binding type 비노출; runtime 내부 socket/queue/dispatch table/adapter는 비공개 | 06 §1.1 | |
| R109 | Root 등록 12항목 표(RouteMesh/ClientServer/fanout/STREAM node/Location Store/Relocation Store/codec/handler·filter/worker/network identity/deployment identity/inbound dispatch) | 06 §2 | |
| R110 | 같은 root 중복 구성·MeshName 중복 등록은 startup 오류; 같은 ChannelName을 다른 topology에 등록해도 오류 | 06 §2 | |
| R111 | Root는 process당 runtime singleton 1개; host 전체 `Relocate`(mode 필수)와 별도 `Shutdown`; `PlannedMaintenance`=같은 버전, `RollingUpdate`=source보다 큰 버전; MeshName/ChannelName/RID별 drain 없음 | 06 §2 | |
| R112 | Framework builder는 liveness interval/deadline 비공개; 공통 profile로 orderly disconnect vs half-open 구분 | 06 §2 | |
| R113 | Framework는 별도 message byte 상한 계산 안 함; Core context가 messaging budget 소유; `CoreHwmMemoryLimitBytes`/`CoreHwmBudgetBytes`/`CoreHwmProfile`(기본 `Balanced`) | 06 §2.1 | |
| R114 | manual budget이 profile보다 우선; 값이 process/container hard limit 초과 또는 비양수면 startup 오류; managed binding은 GC/JVM/V8 hint, native binding은 Core 감지 사용; profile 비율·connection 수 분할 없음 | 06 §2.1 | |
| R115 | Core queue가 binding에 넘기면 byte charge 종료; 이후 payload는 일반 lifetime, HWM credit/capacity token 아님; retained receive로 handler/reply까지 연장 안 함; reply/error reply는 ordinary byte HWM·job queue permit 미사용 | 06 §2.1 | |
| R116 | Application job queue permit 옵션 5개(`ApplicationJobQueueProfile` 기본 `Balanced`, `MaxQueuedApplicationJobs` 1..2,147,483,647, `Pause` 1..100 기본 80, `Resume` 0..99 기본 60(<pause), `EffectiveMaxQueuedApplicationJobs` 읽기전용) | 06 §2.1 | |
| R117 | manual 범위 위반은 startup 오류, unlimited 없음; effective processor 수 계산(logical count/affinity/`floor(quota/period)`(최소1)/executor max의 최솟값, 알려진 값 없으면 1) | 06 §2.1 | |
| R118 | Profile별 processor당 job 수(Compact 32/LowLatency 64/Balanced 128/Throughput 256); 곱셈 overflow는 startup 오류; startup에 확정, runtime 자동 조정 없음; `CoreHwmProfile`과 `ApplicationJobQueueProfile`은 독립 | 06 §2.1 | |
| R119 | pause permit=effective×pause% 올림, resume permit=effective×resume% 내림; `resume<pause` 검증 실패는 startup 오류; pressure count=permit in use(reserved supply+queued job) | 06 §2.1 | |
| R120 | terminal/error reply completion으로 식별되는 pre-receive supply만 permit 미사용; 그 외 ordinary ingress는 receive/claim 직전 shared permit 획득; control/malformed는 내부 처리 직후 반환; application record는 handler turn마다 permit 유지, invocation wrapper가 첫 instruction 직전 정확히 1회 반환; 비동기 대기·continuation은 재획득 안 함 | 06 §2.1 | |
| R121 | 상한 도달 시 cancellable 대기(reject/drop/LWM/polling/busy spin/unbounded 임시 queue 금지); 가장 오래 기다린 live source에 반환 permit 직접 전달, waiter 있으면 새 acquire가 앞지르지 않음; batch/1:N dispatch도 획득 permit 이상 job 생성 불가; Core receive queue 포화는 기존 byte HWM이 처리 | 06 §2.1 | |
| R122 | Framework→Core feedback은 `RUNNING`/`PAUSED` 절대 상태뿐; 이 전이가 Core HWM 설정·queued-byte counter를 안 바꿈; snapshot 투영은 읽기 전용 | 06 §2.1 | |
| R123 | `SessionRelocationSealTimeout` 기본 3,000 ms, finite positive만; 0/음수/무한대/표현 불가 값은 startup 오류; runtime 중 불변 | 06 §2.1 | |
| R124 | Relocation payload 직접 전송 설정 4개(`RelocationPayloadChunkLimit` 256 KiB, `RelocationInFlightPayloadBudget` 16 MiB(0=미적용), `RelocationNodeInFlightPayloadBudget` 0=미적용, `RelocationCutoverWaitTimeout` 1,000 ms) | 06 §2.1 | |
| R125 | chunk가 transport frame 한도 초과 시 startup 오류; 두 budget 합계는 encoded byte 아닌 accounted charge(frame metadata charge 포함) 기준; 배치별 변경 가능, runtime 자동 조정 없음 | 06 §2.1 | |
| R126 | RouteMesh 등록 = MeshName 1개 → MeshNode builder; builder가 소유하는 설정 항목 목록(routing ID/prefix, ROUTER endpoint, ChannelName membership, manual peer, handler, object role, factory, relocation policy, capacity/weight, cache age, publish policy) | 06 §3 | |
| R127 | MeshName은 물리 mesh 이름, ChannelName은 논리 membership; 한 MeshNode에 ChannelName 여러 개 가능; `ChannelName` 호출은 별도 socket 생성 안 함; startup 후 MeshName/RID/endpoint/membership set 불변 | 06 §3 | |
| R128 | `RouteCacheMaxAge` 기본 15초, `MessageFollowDuration` 기본 30초; 둘 다 0이면 끔; 양수면 cache age가 Message Follow보다 최소 5초 작아야 함; 변경은 새 entry/relocation부터; stale route는 stale-location 오류, 자동 재전송 없음 | 06 §3 | |
| R129 | Channel `Client`는 outbound 등록만(광고·weight·handler 없음), `Server`는 membership+handler+weight; Server도 outbound 가능하므로 같은 이름 Client 중복 등록 불가 | 06 §3 | |
| R130 | Channel Server weight 0..10000 기본 100; 실행 중 server weight만 변경 가능; `SetWeight(0)`=drain(Client 의미 아님); `MaxMessageSize` 포함 topology/socket 설정은 startup 후 불변 | 06 §3 | |
| R131 | Object role 닫힌 값(`None`/`Client`/`Server`); `Client`/`Server`는 location store 필수; factory는 Server builder에만, 같은 stable type 중복 등록 시 startup 실패 | 06 §3 | |
| R132 | Object Client는 object 기능만 outbound-only; 같은 MeshNode에 RouteMesh Server 등록 가능하나 Node direct handler는 불가; 양쪽 Client고 Server membership 없을 때만 peer connection 생략; Server membership은 weight 0이어도 연결 필요 | 06 §3 | |
| R133 | Object Server node-wide placement weight 0..10000 기본 100(Channel weight와 독립); 0인 node는 새 placement/relocation target 제외하나 기존 Ready·reservation 유지; 비용 합산 node 전체 제한 없음 | 06 §3 | |
| R134 | Actor/User·Instance Spot 전체·stable type별 limit 기본 0(무제한), 양수는 1..2^31-1, 음수는 startup 오류; Entry Spot은 고정 1개, count 제외되나 Actor 전체 limit엔 포함; Actor stable type별 limit 미제공 | 06 §3 | |
| R135 | 상한 판정은 active+reserved slot; Store가 reservation·authority를 같은 transaction에서 확정; 후보 없으면 `CapacityExceeded`; pending activation 128은 별도 admission 제한(population limit 아님) | 06 §3 | |
| R136 | Activation concurrency 기본 node당 128, 양수만; permit은 factory·init 완료 시 반환, population count 안 바꿈 | 06 §3 | |
| R137 | capacity는 typed bundle 단위 예약(Actor=slot 1, Spot=Spot slot+stable type slot, User Spot aggregate relocation=Spot+type+member Actor 수만큼); 부분 slot 확보는 비공개 | 06 §3 | |
| R138 | 3종 weight(Channel Server/ClientServer Server/placement) 모두 정수 0..10000 기본 100; 음수·10000 초과는 오류; weighted selection은 최소 64-bit 정수로 합계; Logical Multicast는 positive weight 크기 무관 1회 포함, weight 0 제외 | 06 §3 | |
| R139 | Create call은 target RID·predicate·selection callback 미제공 | 06 §3 | |
| R140 | `MaxMessageSize=0`은 Framework 별도 상한 없음; 양수는 같은 byte 상한, 음수는 설정 오류 | 06 §3 | |
| R141 | ClientServer application listener `MaxMessageSize` 기본 16,777,216 bytes(16 MiB); Core budget·job queue와 독립; 0은 상한 없음, 다른 HWM/queue 설정과 결합 검증 안 함; RouteMesh SS에는 미적용 | 06 §3 | |
| R142 | StreamNode Core STREAM inbound 상한 기본 64 KiB; client→server 전체 message header+payload 검사, 6-byte prefix 제외; 0→Core `-1`; server→client outbound 미적용 | 06 §3 | |
| R143 | MeshNode builder에 drain policy·lifecycle command 없음; `Relocating`/`Relocated`/`Draining` 상태 정의; Channel weight 0을 lifecycle state 대용으로 안 씀 | 06 §3 | |
| R144 | Manual peer 2가지 intent(endpoint만/expected RID+endpoint); runtime control은 추가·해제·조회; 같은 MeshName/RID/generation/ChannelName set/security 검증; reconnect는 binding raw socket 계약 사용, application이 loop/backoff 구성 안 함 | 06 §4 | |
| R145 | 메시징 API family 7행 표(Node direct/Channel/Spot/Actor/Logical Multicast/classic fanout/STREAM — 필요 대상, handler namespace) | 06 §5 | |
| R146 | Node direct·channel은 selection+submit 1콜; 공개 `selectNode`/`selectOne`/`selectMany` 단계 없음 | 06 §5 | |
| R147 | Channel client: 인덱스에 없는 이름 `NotFound`(다른 대상 검색·relay 없음); ready pipe 없으면 `Unavailable`, snapshot 자체 없으면 `NotFound` | 06 §5 | |
| R148 | Logical Multicast도 같은 route index로 owner MeshNode 선택; 호출자는 MeshName·endpoint 미제공; 선택된 owner는 monitoring/message-flow에만 남고 application 인자로 안 돌아감 | 06 §5 | |
| R149 | Application 호출은 raw `Message` 대신 업무 객체; raw message는 bindings low-level API·codec extension에만 | 06 §5 | |
| R150 | Call operation 6항목(one-way/session relay는 비동기 admission만, request는 metadata/timeout/취소/typed reply, Logical Multicast는 metadata/ChannelName/topic/비동기 submit, Spot·Actor는 global ID 보존+내부 resolve, ref는 incarnation 변경/bind용, STREAM은 identity·correlation 보존) | 06 §6 | |
| R151 | one-way send·publish·명시적 STREAM reply는 async-only admission; 동기 terminator 미제공; connector send builder는 별도 package 계약; request timeout=reply 대기, send timeout=admission 대기; 즉시 수락되면 scheduler/work queue 추가 없이 resolved awaitable 반환 | 06 §6 | |
| R152 | Metadata는 검증된 immutable snapshot; 같은 key 재설정은 마지막 값; 전체 UTF-8 encoded 1024 bytes 이하; reply는 request metadata 자동 복사 안 함 | 06 §6 | |
| R153 | Logical Multicast: publish 전용 delivery option 없음; bounded I/O executor가 send timeout까지 admission; 실패 시 `DeadlineExceeded`/cancellation/`ShuttingDown` 중 먼저 확정된 것; 시작 후 정확히 1회 처리, cancel/shutdown이 나머지 중단 안 함 | 06 §7 | |
| R154 | target별 결과는 public 결과·monitoring 미반영; snapshot 0개도 정상 완료; 시작 후 실패는 rollback·exceptional completion 없음; 앞 target은 뒤 target 실패로 취소 안 됨 | 06 §7 | |
| R155 | Handler key = owner+message kind(6행 표: Node direct/Channel/Spot packet/Spot subscription/Actor/STREAM session); 같은 key 중복 등록은 startup 오류; 다른 owner엔 같은 packet name 허용 | 06 §8 | |
| R156 | 공유 base context는 MeshName 불요; Channel context는 ChannelName/kind/packet name/metadata/correlation; Node direct context는 MeshName+source·target RID 별도 유지; 선택된 종류·endpoint는 application에 비노출(monitoring만) | 06 §8 | |
| R157 | Runtime reflection 언어는 지정 범위에서 handler 탐색 가능; C++는 compile-time type+명시 등록; 모두 같은 dispatch key·중복 검증 규칙 | 06 §8 | |
| R158 | Handler filter는 root 등록 process-level handler에만 적용; 적용 대상 6행 표(RouteMesh·ClientServer/Node direct/classic fanout=적용, Spot·Actor/Spot 구독/STREAM=미적용) | 06 §8.1 | |
| R159 | Filter context는 dispatch 종류 5값(Channel은 RouteMesh+ClientServer 통합) 제공; RouteMesh·Node direct는 MeshName 제공, ClientServer·fanout은 미제공; 가짜 MeshName으로 fanout 구분 금지; socket 종류·endpoint·dispatch table은 비공개 | 06 §8.1 | |
| R160 | Filter는 등록 순서대로 실행, `next` 호출 시 다음 filter/handler; 완료 후 역순으로 나머지 코드; `next`는 최대 1회, 두 번째 호출은 코드 오류로 거부(자동 재시도 없음) | 06 §8.1 | |
| R161 | `next` 미호출 시 결과 3행 표(Node direct·Channel send=종료, fanout=해당 구독만 종료, request=`Rejected` 오류 reply, `null`을 정상 reply로 안 씀) | 06 §8.1 | |
| R162 | Filter는 request 업무 reply를 직접 만들거나 대체 못함; `next` 미호출 시 filter 반환값이 있어도 `Rejected` | 06 §8.1 | |
| R163 | Handler 실행마다 새 scope; handler·filter instance는 scope당 1회, 같은 scoped dependency 공유; DI lifetime이 이 수명을 안 바꿈; cancellation 신호는 filter와 handler에 동일 전달; 정상/중단/예외/취소 모두 정확히 1회 정리 | 06 §8.1 | |
| R164 | Classic fanout 다중 일치 시 handler마다 별도 dispatch·scope; 한 handler 중단·실패가 다른 handler를 취소 안 함; 이미 시작한 다른 dispatch도 현재 취소로 취소 안 됨 | 06 §8.1 | |
| R165 | Handler 실행 객체·dependency 소유 범위 3행 표(Channel=dispatch 시작~terminal, Spot=activation 시작~종료, Actor=activation 시작~종료) | 06 §8.2 | |
| R166 | 별도 handler class 언어는 activation당 1회 생성, 이후 재사용; DI singleton/scoped/transient가 이 수명을 못 바꿈; 별도 handler lifetime option 없음 | 06 §8.2 | |
| R167 | Spot member function 표현 언어(C++)는 별도 handler object 미추가; Spot method는 Actor별 mutable state를 Spot field에 저장 금지; Actor별 상태는 Actor activation 소유 | 06 §8.2 | |
| R168 | Spot handler 생성자 dependency는 Spot activation scope, Actor handler는 Actor activation scope에서 resolve; 서로 다른 Actor는 mutable handler state·scoped dependency 미공유; `SpotWide`/`PerActor`도 이 규칙 불변 | 06 §8.2 | |
| R169 | 복구 대상 state는 handler field가 아닌 Spot/Actor 소유; handler instance·dependency는 relocation payload에 미포함; Spot/Actor relocation·cross-node Join은 source handler·scope 정리 후 target에서 재생성; same-node Join은 Actor activation 유지(handler·scope 유지); leave/destroy/close도 정확히 1회 정리 | 06 §8.2 | |
| R170 | Activation 종료 시 새 dispatch 먼저 차단 → 실행 중 handler가 terminal 도달 후 정리; 비동기 handler 실행 중 dependency 먼저 정리 금지, 종료 시작된 activation에서 handler 재생성 금지; handler가 종료를 시작해도 순환 대기 금지 | 06 §8.2 | |
| R171 | Mailbox 한도는 건수+대기 byte 합계 두 축 모두 강제, 먼저 걸리는 쪽 적용 | 06 §8.2 | |
| R172 | Byte 회계 = payload+metadata+작업당 고정 비용 합; payload 0이어도 작업은 0 byte 아님; overflow는 최댓값 고정+제출 거절 | 06 §8.2 | |
| R173 | 두 축은 하나의 작업으로 예약(부분 통과 없음); 반환은 handler 완료 후(꺼낼 때 아님); 한도는 대기+실행 중 작업을 함께 셈 | 06 §8.2 | |
| R174 | owner의 scheduler 연속 점유 시간에 상한; 도달 시 남은 작업을 ready로 되돌리고 다른 owner에게 실행 이관; 실행 중인 handler 1개가 상한 초과 실행하는 경우는 계약 범위 밖(handler 경계에서만 확인) | 06 §8.2 | |
| R175 | Scheduler는 도착 기반 wakeup; 주기적 확인을 쓰는 언어는 그 주기를 공표(message 지연 하한); transport readiness는 callback 인자 아님; infrastructure 작업(completion·liveness·admission·relocation·reply recovery·Core HWM 재시도)은 application이 점유 못하는 영역, Actor·Spot lifecycle은 application 실행 영역 | 06 §8.2 | |
| R176 | JSON은 기본 codec; JSON만 쓰면 등록 불요; Protobuf/MessagePack/사용자 codec은 선택 extension으로 root registry 등록 | 06 §9 | |
| R177 | codec extension은 content-type을 parameter 없는 ASCII `type/subtype`으로 등록; media type token 문자만 허용 | 06 §9 | |
| R178 | Registry 검사: SP/TAB 제거→소문자화=canonical form; parameter·내부 공백·non-ASCII·빈 token은 startup 오류; 같은 canonical form 중복 등록은 마지막이 교체 | 06 §9 | |
| R179 | wire에는 canonical form만 기록; 수신은 재변환 없이 registry key와 그대로 비교; 불일치·미등록은 JSON 아닌 `ProtocolError` | 06 §9 | |
| R180 | HTTP client는 media type parameter를 먼저 처리 후 parameter 없는 값만 정규화 절차에 전달 | 06 §9 | |
| R181 | 송신 시 일치 extension 없으면 JSON 선택; 수신 envelope의 non-JSON content-type이 registry에 없으면 JSON 재해석 없이 `ProtocolError` | 06 §9 | |
| R182 | 송신 codec 선택 입력은 호출 지점 선언 type(concrete instance type 아님); base/interface 선언이면 선언 type으로 고름 | 06 §9 | |
| R183 | 여러 조건 동시 충족 시 등록 순서 늦은 것 우선; 아무것도 안 맞으면 JSON | 06 §9 | |
| R184 | 선언 type별 송신 선택 결과 캐시 최대 1,024개; 공간 차면 기존 유지, 새 type은 매번 재평가(미저장) | 06 §9 | |
| R185 | Node.js는 class instance는 constructor를 선언 type으로, base class 다르면 `ZLinkMessage.from(value, declaredType)` 두 번째 인자로 명시; C++는 `add_serializer<TPayload>()`의 compile-time TPayload 사용, runtime type으로 재선택 안 함 | 06 §9 | |
| R186 | 송신 타입 선택 기본값과 수신 wire 검증은 서로 다른 경계 — 같은 fallback 미적용 | 06 §9 | |
| R187 | Codec은 payload bytes 변환만; packet name·routing·correlation·handler 선택은 Framework 소유; 내부 multipart 구조 비공개 | 06 §9 | |
| R188 | 언어별 server root·Stream Connector codec 등록 표면 5행(exact interface owner 표) | 06 §9 | |
| R189 | 두 등록 표면은 같은 typed payload 계약 투영이나 구체 타입까지 동일할 필요는 없음; JSON은 등록 불요, 다른 codec도 instance당 1회 등록 | 06 §9 | |
| R190 | automatic discovery 참여 fanout publisher/endpoint 없는 subscriber/Object Client·Server는 location store 명시 등록; 공식 production store는 Redis extension; 전용 Redis 등록 함수 없음(일반 API로 등록) | 06 §10 | |
| R191 | Redis connection·key prefix는 store instance 생성 시 설정; process-local in-memory store는 contract test에만 | 06 §10 | |
| R192 | Object role `None`+manual peer만은 store 없이 구성 가능; `RecreateOnRelocation`/`PreserveStateWith` factory 또는 Instance Spot factory가 하나라도 있으면 opaque Relocation Store 정확히 1개 필수(same-node join도 예외 없음); Instance Spot factory 없고 모든 factory `DisableRelocation`인 same-node 구성만 생략 가능(cross-node relocation은 capture 전 거부) | 06 §10 | |
| R193 | Location provider가 CAS·reservation·commit·clock capability 미제공 또는 필수 Relocation Store 없거나 2개 이상이면 startup 오류; 두 Store 동시 등록 API·Redis 직접 등록 전용 API 없음 | 06 §10 | |
| R194 | Location Store interface와 Relocation Store interface는 서로 비상속; 각각 독립 registration operation; Actor·Spot별 Store, bundle 등록, Redis 전용 API는 미제공 | 06 §10 | |
| R195 | Classic fanout은 root에서 독립 channel로 등록; Publisher RID 고정 또는 allocation, listener endpoint를 전용 descriptor로 게시; store 없는 publisher는 endpoint를 수동 전달; Subscriber는 automatic/manual 중 하나(혼합 시 startup 오류) | 06 §11 | |
| R196 | Automatic subscriber는 live publisher 전부 연결, publisher마다 전용 SUB socket+receive loop; manual도 endpoint마다 전용 SUB socket; 다른 ChannelName/descriptor 종류/draining publisher/만료 lease는 미연결 | 06 §11 | |
| R197 | Automatic subscriber·RID allocation publisher는 location store 없으면 startup 실패; manual+고정 endpoint는 다른 분산 기능 없으면 store 불요 | 06 §11 | |
| R198 | Fanout handler namespace는 packet name 구분; topic은 context·관측 보존이나 handler 선택 key 아님; subscriber별 topic filter public 설정 미제공 | 06 §11 | |
| R199 | fanout liveness 예약 topic byte `01 5A 4C 46 31`; public publish에 전달하면 호출 인자 오류; beacon은 application에 비노출 | 06 §11 | |
| R200 | Manual subscriber endpoint 집합은 공통 연결 handle 제공(runtime 연결/해제/조회); automatic discovery 결과 수정 불가, mode 전환 안 함 | 06 §11 | |
| R201 | Endpoint 없는 automatic subscriber 상태는 monitoring snapshot/event로만 관찰(읽기 전용, 연결·해제 operation 없음); publisher changed/location changed event는 각각 필수 payload, nullable로 안 섞음 | 06 §11 | |
| R202 | Fanout publish 완료=local transport 수락; subscriber 수신·handler 완료 미확인 | 06 §11 | |
| R203 | Publish 공통 입력(ChannelName/topic/typed event); topic 생략 편의 호출은 packet name을 topic으로; 두 호출 모두 같은 timeout·완료 규칙; subscriber dispatch는 packet name 기준, topic은 context 보존 | 06 §11 | |
| R204 | Fanout publish 비동기 terminator 1개(publisher socket send timeout까지 admission); 정상 완료엔 public 결과값 없음(count 집계 없음); subscriber 0이어도 정상 완료; monitoring에 subscriber 수·수신·완료 정보 없음 | 06 §11 | |
| R205 | Spot·Actor factory는 Object Server builder 등록; User·Instance Spot type은 UTF-8 1..255 bytes case-sensitive stable name(언어 class 이름 미사용) | 06 §12 | |
| R206 | Entry Spot ID는 `<prefix>-entry-<lowercase-canonical-uuid-v4>` 형식으로 Framework가 lifecycle마다 발급(caller 미생성); MeshNode·Entry Spot은 같은 prefix, 각각 별도 UUID; 같은 lifecycle은 같은 ID 유지, replacement lifecycle은 새 RID | 06 §12 | |
| R207 | Entry Spot ID가 global authority와 충돌하면 새 UUID/reservation 없이 `AlreadyExists`로 startup 실패; caller가 User·Instance Spot ID로 예약 형식을 지정하면 startup 오류로 거부; Instance Spot은 Actor handler·membership·Logical Multicast subscription 등록 불가 | 06 §12 | |
| R208 | Actor manager/User Spot manager는 `Create`/`GetOrCreate`/`Find` 제공; Actor는 ID+type 필수, User Spot `GetOrCreate`는 caller 지정 ID+type 필수, `Create`는 Framework가 ID 생성; optional은 initial Mesh/최대 1 MiB creation request/deadline; 같은 option 두 번 설정=startup 오류, terminal submit 두 번=`InvalidOperation` | 06 §12 | |
| R209 | Initial Mesh 생략 시: object role Mesh 1개면 자동 선택, 0개면 `NotConfigured`, 2개 이상인데 미선택이면 `InvalidOperation`; 존재하지 않는 Mesh 명시는 `NotFound`; Instance Spot은 manager create 미제공, Spot direct의 Instance intent만 cold activation 시작; stable type 생략은 distinct type 1개일 때만 자동 선택 | 06 §12 | |
| R210 | Missing Instance Spot: source는 owner claim·reservation을 먼저 안 만듦; target이 activation envelope를 Relocation Store에 immutable recovery root로 저장 후 Location Store 조회+로컬 중복 확인 → 둘 다 없으면 reservation 1개로 권한+capacity 확보 → provider가 fence+recovery root receipt 반환 | 06 §12 | |
| R211 | 경쟁에서 이긴 runtime만 factory·initialize 실행, envelope message를 durable inbox 첫 record로 확정; barrier 유지 중 `Ready` commit(recovery root+cursor 포함) → first record를 queue head로 복원 → barrier open; 진 runtime은 instance 미생성, source는 `Ready` 뒤 재전송 안 함; public call을 check/create로 안 나눔, target node 비노출 | 06 §12 | |
| R212 | Recovery pointer는 첫 handler terminal completion을 durable 기록+cursor 갱신 후에만 Preserve CAS로 제거(queue admission만으로는 제거 안 함) | 06 §12 | |
| R213 | Create는 같은 ID Ready incarnation 있으면 already-exists 오류; GetOrCreate는 `Existing` 반환; Creating attempt 있으면 authority 변경을 deadline까지 대기; 다른 kind·type은 type-mismatch; CAS 패배자는 별도 factory·다른 owner 시작 안 함; creation request는 reservation 전 immutable content reference+hash로 저장; factory는 at-least-once에도 수렴 | 06 §12 | |
| R214 | Actor creation callback 승인=`Created`, 정상 거절=`Rejected`, 예외=`Failed`, recovery cleanup=`Abort`(non-terminal); Creating 대기 중이던 다른 operation은 Ready면 `Existing`, Missing이면 새 reservation 경쟁; 앞선 attempt reply 미공유; 같은 source Node RID·generation·OperationId 재전송만 retained terminal 읽음 | 06 §12 | |
| R215 | Terminal record는 request correlation·reply route 없는 `creation-operation-terminal-v1` semantic envelope 저장, 재전송 reply는 현재 correlation·route로 재encode; `Rejected`/`Failed`는 Ready authority·active capacity 미생성, reserved capacity 반환; Terminal record는 original deadline+5분 TTL | 06 §12 | |
| R216 | Actor·User Spot·Instance Spot factory는 configure callback에서 `DisableRelocation`/`RecreateOnRelocation`/`PreserveStateWith(adapter)` 중 정확히 1개(누락·둘 이상=startup 오류); `DisableRelocation`=cross-node relocation 거부, `RecreateOnRelocation`=payload 없이 재실행, `PreserveStateWith`=Actor는 `ActorRelocationAdapter`, Spot은 `SpotRelocationAdapter`; adapter `Capture`/`Restore`는 opaque byte; Framework는 state contract ID/type/relocation codec 등록 API 미제공; same-node Actor join은 relocation policy 미적용 | 06 §12 | |
| R217 | Relocation ID·target RID·relocation reference·journal cursor·authority revision은 application callback에 비노출 | 06 §12 | |
| R218 | `ActorRef`/`SpotRef`는 global ID+non-zero unsigned 63-bit ObjectGeneration+조회 시점 MeshName·NodeRid; JSON generation은 decimal string; ref는 runtime resource 미소유; bound session accessor는 route switch 뒤 새 immutable snapshot 반환(이전 값 불변); 일반 message는 global ID로 resolve, ref location을 target으로 고정 안 함; Destroy/Close는 exact ref 받음, incarnation 없으면 `false`/generation 다르면 `InvalidOperation`/seal 중이면 `Unavailable`(다른 incarnation 종료 안 함) | 06 §12 | |
| R219 | Manager `Find`는 current Ready ref 반환; Actor 소속 조회도 current `SpotRef`만; Location operational query는 page size 1..1000, encoded page 최대 4 MiB; public handle/directory/resolver/unbounded list 미제공 | 06 §12 | |
| R220 | Actor factory는 lifecycle 생성, handler는 Actor context registry 등록; Actor message는 mailbox 직접 dispatch(Node/Spot packet handler 재분류 안 함) | 06 §12 | |
| R221 | `Yield`는 Channel/Spot/Actor request, CPU·I/O worker call, create/get-or-create call에 제공(join/send/publish/timer 등록/close/destroy 미제공); `SpotWide` Spot 또는 Instance Spot shared gate에서만 유효; 지원 안 하는 문맥이면 admission·queue 변경·turn 반납 없이 `InvalidOperation` | 06 §12 | |
| R222 | `SpotWide` member Actor의 `Yield`는 User Spot gate만 반납, 현재 Actor queue head 실행 권한은 유지 — 다른 handler는 진행 가능하나 같은 Actor 다음 job은 대기; 같은 Actor 자신에게 보낸 request도 큐 우회·inline 실행 없음 | 06 §12 | |
| R223 | Spot direct 시작 method는 global ID+payload → send/request call; Instance intent 있는 call은 resolve+claim을 하나의 terminal operation으로; existing authority 있으면 저장된 kind·type·현재 Mesh 사용, cold activation option이 current owner를 이동 안 시킴 | 06 §12 | |
| R224 | STREAM node는 MeshNode와 독립 등록 가능; Session·Actor binding 사용 시 STREAM session service가 raw STREAM↔MeshNode 관계 소유; ingress는 bound Actor mailbox, egress는 bound session FIFO; Actor dispatch 활성화 설정은 MeshName 미수신; startup 시 같은 root에 Object Client/Server role+location store 1개 이상 필요 | 06 §12 | |
| R225 | 언어별 exception/error object는 공통 13개 `ErrorKind` 사용, 재시도 여부 미추가 | 06 §13 | |
| R226 | Operation 결과 변환: Node direct는 RID, Spot·Actor는 global ID, session binding은 exact generation+binding token 유지; 물리 peer generation은 public commitment 아님 | 06 §13.1 | |
| R227 | select-one ChannelName은 첫 binding operation 시작 직전에만 선택; 시작 전 route eligibility·admission 확인 단계에서만 재선택 가능; 시작 후 Core가 HWM 재시도·완료 소유, target 재선택·재제출 없음 | 06 §13.1 | |
| R228 | Operation 결과 변환 8행 표(source outbound admission 수락/일반 one-way 첫 submit/Logical Multicast 일부 실패/route 미준비/authority·경로 없음/필터·정책 거부/shutdown/invalid argument) | 06 §13.1 | |
| R229 | `DeadlineExceeded`는 family별 send timeout 미수락 시 Framework가 만드는 exception; cancellation은 cancelled awaitable; invalid arg/handle/state·재사용 reply token·중복 terminator는 exceptional completion | 06 §13.1 | |
| R230 | STREAM reply 첫 terminator가 admission 전 원자적으로 token 소비; backpressure/timeout/cancel 완료돼도 재사용 불가; 두 call 경쟁 시 하나만 admission 시작 | 06 §13.1 | |
| R231 | Direct pending one-way operation은 Node RID/global ID/binding token 유지; 첫 binding operation 시작 시 target 확정, Core가 HWM 재시도 소유; detach·timeout은 terminal, route 재조회·replay 없음; select-one target 선택도 같은 시작 경계 적용 | 06 §13.1 | |
| R232 | Global object message 결과 구분 7행 표(Actor/Spot one-way·request, exact ActorRef bind/destroy, exact SpotRef close — missing authority/route unavailable/generation mismatch/pre-commit seal) | 06 §13.1 | |
| R233 | Create·GetOrCreate: eligible node 없거나 capacity 부족은 `CapacityExceeded`, reservation owner route 미준비는 `Unavailable`; Store 실패는 `InternalFailure`; kind·type 충돌은 `TypeMismatch`; stale authority는 `Unavailable`; creation callback 정상 거부는 typed `Rejected`(다른 owner 자동 재제출 없음) | 06 §13.1 | |
| R234 | request 실패는 확인 시점 무관 해당 kind로 1회 완료; one-way는 local outbound admission 전 확인한 경우만 exceptional completion, 수락 후 실패는 이미 완료된 call을 안 바꿈(drop metric+message-flow record로만 관측) | 06 §13.1 | |
| R235 | Request admission 뒤엔 typed reply/typed error/timeout/cancellation/shutdown/protocol 오류 중 하나만 terminal; generation 충돌은 stale 결과, busy/capacity는 admission 오류; 자동 재제출 없음; caller cancellation 뒤 늦은 transport completion은 correlation만 정리, 두 번째 terminal 없음 | 06 §13.1 | |
| R236 | Dispatch 실패 structured record의 reason/action/caller 결과 대응은 `26-message-flow-tracing`(observability 주제) 단일 소유; 언어별 integration은 닫힌 값을 그대로 기록(추가·축소 없음); public event DTO·observer enum 미제공 | 06 §13.2 | |
| R237 | Startup validation 21항목 목록(중복 검사, RID·endpoint, Client·Server 역할 중복, Object Client 조합, ClientServer 역할·store, 송신 경로 중복, handler key 중복·누락, channel·handler 종류 일치, store 등록, manual peer 형식, fixed RID 조건, ASCII prefix 규칙, role·target 일치, factory·owner 관계, stable type 중복 등 …) | 06 §14 | |
| R238 | 설정 오류는 lazy first call까지 미루지 않고 startup 실패 | 06 §14 | |
| R239 | Runtime query는 DI 사용 가능 public service; 반환 항목(MeshNode status, peer admission, membership·weight, role·weight·capacity, server readiness, bounded location page, lifecycle state·backlog)은 caller-owned snapshot | 06 §15 | |
| R240 | Monitoring event 제공 필드(source kind/ChannelName/조건부 MeshName·server identity/lifecycle generation/구조화 오류); 값 종류 많은 식별자(topic/Actor ID/Spot ID)는 metric label 미사용 | 06 §15 | |

### `framework-error-model` (R241~R266)

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| R241 | 이 문서는 `Send`/`Request`/lifecycle operation 실패의 공통 오류만 정의; 내부 함수·상태기계 단계 아님 | 32 §1 | |
| R242 | 오류에 재시도 여부 미포함; application이 완료 조건·idempotency·업무 상태 확인 후 새 operation 결정 | 32 §1 | |
| R243 | 공통 `ErrorKind` 13개 값과 번호(0 `NotFound` ~ 12 `InternalFailure`, 값 0도 유효) | 32 §2 | |
| R244 | Generation/owner fence/moving phase/worker queue 상태/relocation 단계는 내부 원인 — 별도 대응 불필요하면 새 public kind로 미노출, log/trace에만 기록 | 32 §2 | |
| R245 | 호출 위치에서 바로 확인 가능한 오류(잘못된 인자·종료된 handle)는 표준 argument/invalid-operation 오류; startup configuration 오류는 언어별 configuration exception; remote error reply로 미변환 | 32 §3 | |
| R246 | Outbound queue 수락·route resolve·remote reply 대기 중 확인한 실패는 언어별 Framework exception 또는 `result`의 `ErrorKind` | 32 §3 | |
| R247 | `Send`는 outbound queue 수락 시 완료(target handler 처리 의미 아님) | 32 §4 | |
| R248 | Send 완료 전 확인 조건 4행 표(target·route 없음→`NotFound`, connection·owner 불가→`Unavailable`, timeout까지 미수락→`DeadlineExceeded`, admission 중단→`ShuttingDown`) | 32 §4 | |
| R249 | Send 완료 후 실패는 이미 완료된 call 결과를 안 바꿈; metric/log/trace로 기록, 다른 target 자동 재제출 없음 | 32 §4 | |
| R250 | `Request`는 typed reply 수신 시 정상 완료; 실패는 다음 중 하나로 1회: `NotFound`(대상·handler 없음), `Unavailable`(route·connection·owner 불가), `DeadlineExceeded`(deadline 내 미수신), `ProtocolError`(wire·payload·reply type 처리 불가) | 32 §5 | |
| R251 | `CapacityExceeded` = source가 소유한 local bounded resource(reply 보관 자리, operation table entry, 같은 process 내 Spot·Actor 대기열) 미확보 | 32 §5 | |
| R252 | `Unavailable` = 다른 node의 대기열 포화(target 대기열 상태를 `CapacityExceeded`로 표현 안 함); 구분 기준은 "실패한 자원을 이 runtime이 소유하는가" | 32 §5 | |
| R253 | 이 구분은 대기열에만 적용; target node의 배치 수용량 부족은 admission 판정이므로 `CapacityExceeded`가 맞음 | 32 §5 | |
| R254 | Message Follow relay queue·relocation ingress hold에는 relocation이 정하는 record 수·byte 상한이 없음; 보관량 증가만으로 `CapacityExceeded` 미반환; 단일 message 크기 상한·transport·deadline·cancellation 제한은 그대로 적용; execution lane 수락 후엔 lane reservation 적용하되 이를 relay queue·hold 보관 상한으로 미사용; 이 제한으로 실패하면 소유 runtime 기준으로 오류 결정 | 32 §5 | |
| R255 | Runtime 종료 중이면 `ShuttingDown`; 표현 불가한 실패는 `InternalFailure` | 32 §5 | |
| R256 | Cancellation은 cancelled awaitable; `DeadlineExceeded`·cancellation은 caller가 대기를 그만뒀다는 뜻이지 remote handler 미실행을 뜻하지 않음; 뒤늦은 reply로 두 번째 결과 미생성 | 32 §5 | |
| R257 | `Accepted`/`Rejected` typed 결과가 있는 operation(Actor create·join)은 application 판정을 typed 결과로 반환; 이때 `Rejected`는 Framework exception 아님 | 32 §6 | |
| R258 | 공통 `ErrorKind.Rejected`는 typed 결과 없는 filter/admission/runtime policy 거부에만 사용; application 업무 규칙 거부를 exception으로 변환 안 함 | 32 §6 | |
| R259 | Public exception/error object/typed failure에 재시도 hint(`RetryAdvice`/`isRetriable`/`retriable`) 미포함(같은 kind라도 실행 여부·중복 영향이 다를 수 있음) | 32 §7 | |
| R260 | Application이 새 operation 시작 전 확인할 3단계(완료 조건·remote 실행 가능성, idempotent 여부/idempotency key, 필요 시 업무 상태 재조회) | 32 §7 | |
| R261 | 하나의 binding operation 안 Core 소유 HWM 재시도는 application retry 아님; Framework는 send-ready waiter 미두고 다른 logical target에 자동 제출 안 함 | 32 §7 | |
| R262 | 5개 server package+HTTP client package가 같은 13개 kind 사용; 언어별 interface는 enum 이름·exception·result 표현만 정의, kind 추가·재시도 boolean 추가 금지 | 32 §8 | |
| R263 | Contract test·E2E 검증 5항목(kind·숫자 일치, Send 완료 시점 불변성, Request timeout/cancellation 뒤 늦은 reply 무시, typed `Rejected`와 exception 구분, 재시도 hint 부재) | 32 §8 | |
| R264 | Manual queue 값이 `1..2,147,483,647` 밖이거나 overflow면 socket bind 전 configuration error | 32 §9 | |
| R265 | Runtime shared-cap 부족은 public error·typed reject·drop 사유가 아니라 cancellable wait | 32 §9 | |
| R266 | Owner 구조 한도 위반만 기존 owner error 사용, 두 조건을 안 섞음 | 32 §9 | |

### `layering` (R267~R301)

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| R267 | 모든 언어 구현이 같은 책임 그래프(binding type 이름·package 배치·문법이 아닌 의미의 소유·runtime 비용으로 정의) | 40 §1 | |
| R268 | Framework는 public contract/semantic runtime core/binding-facing integration 3층; public contract·core에 binding type 비노출 | 40 §1 | |
| R269 | Integration 영역은 binding 동작 의미·소유권·수명·준비 상태·오류가 이미 계약과 같으면 binding API 직접 호출; 다를 때·여러 binding object 결합 필요할 때만 adapter/port | 40 §1 | |
| R270 | `SpotNode`/`Stream`은 결합 예시일 뿐 특별 면제 아님 — 같은 POSDDD·성능 검토 통과 필요 | 40 §1 | |
| R271 | Binding public type은 integration 영역에서만 참조; 추상화 모양 맞추려 domain contract에 복사 금지; adapter도 runtime 구현이므로 Framework가 정할 동작을 binding package로 이전 금지 | 40 §1 | |
| R272 | 동작 분류 표 5행(계약 이미 만족?/소유권·수명 동일?/준비상태·오류 동일?/동시성 규칙 동일?/하나의 binding object가 전체 제공?) — 각 답에 따라 직접 사용 vs adapter | 40 §1 | |
| R273 | adapter 필요 조건 4가지(결과 매핑, 정해진 순서 close, 수신 storage 재사용, 여러 binding object를 하나의 동작으로 결합); binding type이 외부 package에 있다는 사실만으로는 불충분 | 40 §1 | |
| R274 | POSDDD 검토 관문 6항목(깊은 모듈/정보 은닉/복잡성 하향/오류를 정의로 축소/bounded context/두 설계 비교); 숨길 결정 없는 adapter는 제거 | 40 §1 | |
| R275 | 성능 관문 6항목(receive storage 재사용, 불필요한 복사·이중 변환 금지, wrapper·collection·task 남발 금지, 이중 lock 금지, poll event storage 재사용, throughput/p99/allocation/lock contention 측정) | 40 §1 | |
| R276 | 금지 구조 5가지(`*Wrapper` class, test·가상 backend 전용 `IBackend*`, binding 재노출 facade, binding public API 우회 reflection/internal 접근/raw-frame 우회, 다른 언어 구현 세부만 근거로 한 언어별 public API) | 40 §1 | |
| R277 | 언어별 class 이름·file layout은 달라도 되나 책임 그래프·public 동작·소유권 규칙·lifecycle 순서·성능 기대치는 동일; binding이 동작을 표현 못하면 차이를 기록(private wrapper·raw-frame·언어 전용 public contract로 보상 금지) | 40 §1 | |
| R278 | 정본 판정은 허용 의존 그래프+POSDDD 검토+성능 관문의 결과(type 이름 검색은 보조 수단일 뿐) | 40 §1 | |
| R279 | 언어별 재량 — interface/추상 class/protocol/함수 묶음/binding 직접 호출 중 선택 자유, adapter 파일 분할 자유; 공통 규칙은 public binding API만 사용·wrapper 미생성·소유권/의미변환/측정한 비용 명시 | 40 §1 | |
| R280 | process에 host runtime 1개, topology별(RouteMesh/ClientServer/fanout/STREAM) runtime은 그 아래; **종료 순서는 host 소유, resource 닫는 방법은 그 resource를 만든 topology 소유** | 40 §2 | |
| R281 | host는 공통 lifecycle 절차(수락 중지→drain→close)를 정해진 순서로 호출; 각 topology는 그 호출에 자기 resource를 닫음; 여러 번 호출해도 결과 동일(idempotent) | 40 §2 | |
| R282 | 종료 경로가 구체 타입 검사로 분기 금지(topology 추가마다 분기 증가·같은 resource가 경로별로 다른 순서로 닫히는 문제 방지) | 40 §2 | |
| R283 | 종료 조율을 host 통합 package(웹 프레임워크 통합 등)에 두지 않음 — 통합 계층은 runtime 시작·종료를 host lifecycle에 연결만, 정리 순서는 runtime이 소유 | 40 §2 | |
| R284 | 같은 프로토콜 스택을 client 라이브러리와 framework가 각자 구현 금지 — 한 곳에서 구현하고 양쪽이 사용 | 40 §2 | |
| R285 | 관찰 기준 — topology resource 개별 close 호출이 host 종료 절차를 건너뛰지 않음 | 40 §2 | |
| R286 | 종료 의도 2가지(이전 후 종료=배포·축소 등 계획된 종료, 즉시 종료=급한 종료·미이전) | 40 §3 | |
| R287 | 같은 종류 요청이 겹치면 조건이 같으면 진행 중 절차에 합류, 다르면 `Blocked/OperationInProgress`로 즉시 거절(대기 없음) | 40 §3 | |
| R288 | Relocate와 Shutdown이 겹치면 shutdown이 이기고 relocation 대기 쪽은 `Blocked/ShutdownRequested` | 40 §3 | |
| R289 | 이전 후 종료는 상태를 바꾸기 전에 host 전체를 한 번에 검사; 받을 node가 당장 없어도 즉시 거절하지 않고 정해진 시간까지 대기 후 `Blocked/TargetUnavailable`; 거절 결과는 저장하지 않아 재요청 시 처음부터 재검사 | 40 §3 | |
| R290 | 옮길 대상이 하나도 없으면 받을 node 없이도 성공; host 상태 전이·새 작업 차단은 다른 이전과 동일 | 40 §3 | |
| R291 | 첫 이전 확정 전 실패는 원래 상태로 복귀; 확정 후 실패는 이미 옮긴 것은 유지, 나머지만 재처리 후 `Serving` 복귀; 종료는 caller가 별도 요청해야 발생 | 40 §3 | |
| R292 | 관측 구독자가 응답하지 않아도 종료 절차는 진행(구독자가 진행을 못 막음) | 40 §3 | |
| R293 | 정리 순서 고정 — resource는 만든 쪽이 닫음; 자식은 부모 resource 사용 중 부모가 안 닫혔음을 보장하는 참조 보관; 밖으로 나가는 참조는 closed 여부·generation 일치를 확인 가능해야 함 | 40 §4 | |
| R294 | 정리 9단계 순서(신규 작업 차단→위치 저장소에 종료 게시→기존 작업 마무리→종료 callback 실행→timer·session·관측 정지→peer 연결·endpoint·전송 콜백 정지→실행 대기열 비우기/취소→provider·전송 자원 close→최종 상태·구독자 완료) — **4번이 5번보다 먼저**(callback이 아직 유효한 소속·인스턴스에서 실행돼야 함) | 40 §4 | |
| R295 | 최종 결과 게시 뒤에는 새 callback·timer·완료·이벤트를 시작하지 않음 | 40 §4 | |
| R296 | 등록 선언은 시작 시점에만 검증, 통과 후 불변(실행 중 재검증 없음); 검증 대상은 시작 전에 알 수 있는 모순(이름 중복·handler 없는 channel·배타적 옵션 조합); 검증 실패 시 일부만 등록된 채로 시작 안 함 | 40 §5 | |
| R297 | 식별자 8종과 유효 범위 표(mesh 이름/node RID/node lifecycle generation/channel 이름/객체 ID/객체 세대/진행 중 호출 식별자/물리 연결 식별자) — 서로 합치지 않음 | 40 §6 | |
| R298 | 진행 중 호출 식별자 유일성은 값 하나가 아니라 `(보낸 node RID, lifecycle generation, 호출 식별자)` 조합으로 확보(재시작 후 번호 재사용으로 인한 오배정 방지); 값의 길이·내부 형식은 비공개 계약 | 40 §6 | |
| R299 | 진행 중 호출 식별자 형식은 runtime 내부에 하나만; 식별자는 각각 전용 타입, 표기 하나로 통일(문자열 변환은 경계에서 1회만); `OperationId`(Actor Join 완료 중복 처리 값, 용어집 항목)와 이름 충돌 금지 — internals는 다른 이름 사용 | 40 §6 | |
| R300 | 물리 연결 식별자·저장소 record version·실행 queue 내부 순번은 공개 DTO에 미포함(runtime이 같은 대상 재확인용으로만 사용) | 40 §6 | |
| R301 | 확인할 결과 19항목 체크리스트(§7 그대로 — binding type 비노출, wrapper 없음, adapter 조건, 성능 측정, 종료 절차 준수, 정리 순서 고정, 검증 시점 등) | 40 §7 | 검증 |

## 6. 링크·코드·site 영향

이 주제는 모든 주제 가운데 참조 규모가 가장 크다 — 특히 `01-glossary`(912 anchor, 91개
문서)와 `06-framework-api`(44 anchor, 37개 문서).

| 대상 | 처리 |
|---|---|
| 스펙 내부 링크(글로서리 anchor 912개 포함, 총 8개 문서에 걸린 anchor 980개, 파일 기준 122개
  고유 문서) | 새 경로·새 절 번호로 치환. 절 제목은 대부분 유지되지만 §3.5(framework-api §15~18
  분리)·§3.7(layering 라벨 제거)로 anchor 문자열이 바뀌는 절이 있으므로 이 두 문서는 **anchor
  치환표를 별도로 만든다** |
| glossary 항목 anchor(`#spot`, `#actor` 등 `<a id>` 129개) | §3.0에서 정의 표만 남기고 절차
  문단을 삭제해도 `<a id>`와 `### 용어` 제목은 그대로 유지 — 외부에서 이 anchor로 들어오는
  912개 링크가 깨지지 않게 하는 것이 최우선 제약. 절 안의 내부 소제목만 사라짐 |
| cpp layout contract test | `test_cpp_framework_layout_contract.cpp`의
  `framework_api_documents_actor_destroy_lifecycle`이 확인하는 문장 4개(§1)는 §3.5의 절
  15(factory 등록)·§13.1(오류 표)로 옮겨진다. needle 문자열은 원문 그대로 보존하고, 경로만
  `00-foundation/06-framework-api.ko.md`로 갱신(이동은 캠페인 마지막 단계, §5 원칙) |
| `verify-framework-submit-api.sh` | §7(Logical Multicast 완료) needle 3개 보존, 절 번호만 변경 |
| `verify-framework-instance-spot-contracts.sh` | §12(재구성 후 §15~17) needle 9개 보존 |
| Java 소스 주석 4곳 (`ZLinkSpotLifecycle.java` 등) | `01-glossary`를 줄 번호 없이 이름으로만
  인용하므로 경로 이동에 영향받지 않음. 재구성 뒤 절 번호가 바뀌면 주석이 stale해질 수 있으나
  이 캠페인은 코드를 건드리지 않으므로(§ 지침 3) 갱신하지 않고 그대로 둠 — 필요하면 별도
  후속 작업으로 기록 |
| Node 소스 주석 다수 (`framework-errors-internal.ts` 등 20여 곳) | `32-framework-error-model`을
  `줄 번호:줄 번호` 형식으로 인용. 절 재구성으로 줄 번호가 전부 바뀌므로 코드 갱신이 필요하지만
  이 캠페인 범위 밖(코드 미변경) — spec-gap 후보 아님, 별도 실행 항목으로만 기록 |
| mkdocs nav | "Foundation" 그룹 → `00-foundation/README`, 01~08 8개 문서 |
| redirect | 캠페인 말미 site 작업에서 `00-public-contract-governance`→`00-foundation/01-…`,
  `01-glossary`→`00-foundation/02-glossary`, … 8개 매핑 |
| 검증 | `check_doc_links.py`, `check_doc_tabs.py`, `mkdocs build --strict`, `git diff --check`,
  cpp layout contract test, `verify-framework-submit-api.sh`, `verify-framework-instance-spot-contracts.sh` |

## 7. 구현 대조 — 에이전트 입력

재작성이 끝나면 언어별 에이전트 4개(dotnet·jvm·cpp·node)에 다음을 준다.

- 입력: 새 8개 문서와 §5 대장(새 위치 열 채운 것). Glossary는 RG1~RG8(구조 규칙)만 대상 —
  용어 정의 자체는 대조 대상이 아니다(정의는 계약이 아니라 이름 붙이기이므로 "구현과 일치"를
  판정할 대상이 없다).
- 과제: 대장의 행마다 해당 언어 구현에서 **일치 / 불일치 / 스펙 미정 / 판단 불가** 중 하나와
  근거(파일:줄)
- 특히 확인할 것: `06-framework-api`의 수치·기본값(§2.1의 profile·budget·chunk 설정, §3의
  weight·capacity 상한)과 `32-framework-error-model`의 `CapacityExceeded`/`Unavailable` 구분
  기준(R251~R254) — 세션 파일럿에서도 이런 수치·경계 규칙이 실제로는 언어마다 흩어져 있는
  사례가 나왔다
- 금지: 스펙 수정, 코드 수정. 판정은 하지 않고 사실만 보고
- 출력 양식: `| R# | 판정 | 근거 | 비고 |`

판정(Claude): 옛 문서·4개 구현이 일치하는데 새 문서만 다르면 **문서 오류** → 수정. 옛
문서 때부터 달랐거나 언어끼리 다르면 **spec gap** → [spec-gap 대장](../../spec-gap.ko.md) 등록,
문서는 계약 의도대로 유지.

## 8. 작업 순서

8개 문서, 서로 링크가 촘촘하다(`interaction-model`↔`message-model`↔`framework-api`가 서로
참조하고, 전부 `glossary`를 참조한다). 세션 파일럿처럼 절 제목을 프롬프트에 고정 문자열로
박아 두면 병렬로 진행할 수 있다.

1. `README.ko.md` 초안(§2 질문표 기준) — 8개 문서의 최종 절 제목을 여기서 먼저 확정해 아래
   병렬 작업에 고정 문자열로 나눠 준다.
2. **1차 병렬(4개 에이전트)** — 상호 의존이 적고 분량이 작거나 중간인 문서:
   `public-contract-governance`(R1~R22), `overview`(R23~R39), `message-model`(R81~R107),
   `framework-error-model`(R241~R266). 이 넷은 서로를 깊이 참조하지 않는다.
3. **1차와 동시에(1개 에이전트)** — `layering`(R267~R301). 다른 7개 문서와 내용 의존이 거의
   없는 구현 스펙이므로 독립적으로 진행하되, **결정 라벨 제거(S2)와 "정본" 교체(S3)**를
   최우선 처리 항목으로 지정한다.
4. **2차 병렬(2개 에이전트)** — `interaction-model`(R40~R80)과 `framework-api`(R108~R240).
   가장 크고 서로 참조가 많으므로 1차가 끝나 절 제목이 고정된 뒤 시작한다. 두 문서가 서로
   링크할 절 제목(예: "Send와 request", "Call operation")을 프롬프트에 고정 문자열로 명시한다.
5. **마지막(1개 에이전트) — `glossary`(RG1~RG8 + §3.0 A/B/C 판정).** 다른 7개 문서의 최종 절
   구조가 확정되어야 "이 절차는 이미 X 문서 Y절이 소유한다"는 링크를 정확히 걸 수 있다.
   따라서 glossary는 2~4단계가 완료된 뒤 시작한다. 이 주제 밖(다른 주제) 문서를 가리켜야
   하는 항목(§3.0 갈래 C)은 옛 경로로 링크해 둔다.
6. 등가성 대조 — 대장 빈 행 0, 추가 보장 0(§5 전 R행 + RG행).
7. en 짝 작성(캠페인 §5, 이번 주제에서는 하지 않음).
8. 링크 치환·guide 재생성·nav·cpp test·verify 스크립트 갱신 → 검증(§10) 4종 + verify 스크립트
   2종 그린.
9. 구현 대조(§7) → 판정·기록.
10. 한 커밋(문서 이동+내용) + spec-gap 대장 갱신.

## spec-gap 후보

이번 읽기에서 발견한, 재작성이 아니라 판정이 필요한 항목이다. 고치지 않고 위치만 기록한다.

- **G-F1** `00-public-contract-governance.ko.md` §8 "11.0 spec-first 정본 규칙"이 계약 문서
  안에 일회성 마이그레이션 정책(Core service 이관, 11.0 특정 시점)을 담고 있다. 가이드는
  "Internals는 migration 이력이나 진행표를 포함하지 않는다"고 하는데 이 절은 계약 문서에
  마이그레이션 서술이 섞인 사례다. 11.0 이관이 끝났다면 이 절을 삭제하거나 실행 ledger로
  옮기는 것이 맞는지, 아직 진행 중이라 계약 문서가 임시로 소유해야 하는지는 판단이 필요하다
  (§00-public-contract-governance.ko.md §8).
- **G-F2** `06-framework-api.ko.md` §2.1(Core memory budget·Application job queue)의 permit
  획득/반환 순서, mailbox 2축 회계, batch/1:N dispatch 규칙(R120~R122)이 실행(execution) 주제로
  예정된 `33-core-hwm-application-job-flow.ko.md`가 이미 상세히 소유하는 내용과 상당히 겹친다.
  이 문서 자체도 "두 capacity의 분리 의도 … 는 33이 정의한다"고 링크하지만, 그 앞에 이미
  행동 규칙 문단이 여러 개 있다. 00-foundation과 01-execution 주제 경계를 어디서 그을지는
  01-execution 매핑표 작성 시점에 함께 판정해야 한다(계약 변경이 아니라 소유 문서 재배치
  판단이므로 spec-gap이 아니라 캠페인 내부 조율 항목일 수 있음 — 코디네이터 확인 필요).
- **G-F3** `06-framework-api.ko.md` §14 startup validation 목록과 `40-internal-layering.ko.md`
  §5(등록 선언 검증)가 "시작 시점에만 검증, 통과 후 불변"이라는 같은 원칙을 서로 다른 각도
  (공개 계약 목록 vs 구조 원칙)에서 말한다. 내용 충돌은 없으나 재작성 시 한쪽이 다른 쪽을
  링크하도록 명시할지 판단 필요(모순은 아니므로 spec-gap이 아니라 §4 구조 문제로 처리해도
  무방 — 코디네이터가 재작성 리뷰에서 결정).
