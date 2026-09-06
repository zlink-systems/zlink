# 스펙 심층 리뷰 (D-102) — 기준과 findings 양식

> 목적: 0.17.0 재검증(`seven-samples-green-v2`, `4e8182af01`) 뒤 스펙 전체를 읽기 전용으로 진단한다.
> 산출물은 findings 보고서뿐이다. 스펙·코드는 수정하지 않는다(감독자가 triage 뒤 `decisions.ko.md`에
> D-1xx로 기록하고 스펙은 감독자가 직접 편집한다).

## 리뷰 기준

각 finding은 아래 분류 중 하나로 표시한다.

| 분류 | 묻는 질문 |
|---|---|
| `complexity` | 이 규칙·상태·분기가 계약을 설명하는 데 꼭 필요한가? 없애도 관찰 가능한 결과가 같은가? |
| `lower-layer-reverification` | 상위 계층(binding→Core, framework→binding/Core)이 하위 계층이 이미 보장하는 것을 다시 검증·추적·재시도하는가? (예: Core가 pair 종료 시 NOT_CONNECTED를 즉시 주는데 framework가 별도 timer로 같은 것을 판정) |
| `consolidation` | 같은 결과를 내는 규칙이 둘 이상인가? 하나의 규칙 문장으로 합칠 수 있는가? (규칙 수 before→after를 센다) |
| `scattered-control` | 한 판정(예: admission 허용, endpoint 교체, deadline 소유)이 여러 문서·여러 계층·여러 코드 위치에 흩어져 있는가? 소유자를 하나로 정할 수 있는가? |
| `parity-gap` | 같은 공통 규칙을 언어별 구현·언어별 문서가 다르게 구현·서술하는가? |
| `spec-impl-drift` | 스펙 문장과 구현이 다른가? 어느 쪽이 계약인가(spec-writing-guide §4.4: 계약 서술이면 코드를, 구현 서술이면 문서를 고친다)? |
| `gate-drift` | 문서 계약 검증 스크립트·contract test가 스펙과 어긋나 신호를 잃었는가? |
| `form` | spec-writing-guide 위반: 같은 규칙이 여러 층에 중복(§4.3), 검증 요구 절에 white-box 서술(§9.3), 규칙 라벨·압축 명사구(§2.4/§3.5), 용어집 미등록 용어(§3.4) |

원칙(캠페인 규칙과 같다): 규칙은 소유자 하나. "단순"은 규칙 수가 적은 것이다. 완화책(timeout·retry·
두 번째 poller·catch-all)을 규칙으로 제안하지 않는다. 제안은 항상 "규칙 N개 → M개"로 표현한다.

기준 문서: `doc/principal/documentation/spec-writing-guide.ko.md`(특히 §1 독자 질문, §4.3 층 분리,
§4.4 계약/구현 서술, §9.3 검증 요구), `doc/principal/documentation/documentation-principles.ko.md`.
최근 바뀐 규칙: `doc/plan/c016-worklog/decisions.ko.md`의 D-090~D-101(2026-09-05/06).

## Findings 양식(고정)

보고서 첫 부분에 요약 표(번호 · 제목 · 분류 · 행동 변경 · 규칙 수 · 성능 영향 · 확신)를 두고, 그 아래
finding마다 다음 항목을 같은 순서로 쓴다. 항목을 빼지 않는다(해당 없음이면 `없음`).

```markdown
### F-<job>-<n> <짧은 제목>
- 분류: complexity | lower-layer-reverification | consolidation | scattered-control | parity-gap | spec-impl-drift | gate-drift | form
- 위치: <spec file:line> (규칙이 서술된 모든 위치; 여러 문서면 모두)
- 현재 규칙(인용): <원문 그대로, 필요한 만큼만>
- 문제: <무엇이 중복·과잉·흩어짐·불일치인지 한 문단>
- 제안: <단일 소유자 문서·절과 통합 후 규칙 문장(초안). 삭제면 "삭제">
- 규칙 수: before N → after M
- 행동 변경: 없음 | 있음 — <application이 관찰하는 차이 한 문장>
- 영향: core | bindings(<언어들>) | framework(<언어들>) — 구현 파일 file:line
- 성능 영향: 없음 | 있음 — <hot path 어디에서 무엇(검증·복사·timer·lock·상태 갱신)이 사라지는지>
- 근거 코드: <구현 file:line 2~5개, 언어별로>
- 확신: 높음 | 중간 | 낮음 — <낮으면 무엇을 더 확인해야 하는지>
```

우선순위는 (1) `lower-layer-reverification`·`scattered-control` 중 행동 변경 없이 제거 가능한 것,
(2) `consolidation`, (3) `spec-impl-drift`·`parity-gap`, (4) `form` 순으로 요약 표를 정렬한다.
job당 finding 상한 20개. 상한을 넘으면 낮은 순위를 "추가 후보(요약 1줄)" 절에 모은다.

## Job 분할

| Job | 범위(읽기) | 구현 근거 |
|---|---|---|
| R1 core-api | `core/doc/spec/core/{README,00..08,glossary}.ko.md`, `core/doc/spec/core/socket/*.ko.md` | `core/src/**` |
| R2 core-protocol-systems | `core/doc/spec/core/protocol/*.ko.md`, `core/doc/spec/core/systems/*.ko.md` | `core/src/**` |
| R3 bindings | `bindings/doc/spec/**/*.ko.md` | `bindings/{c,cpp,java,node,dotnet,go,rust,python}/**` |
| R4 fw-foundation-execution | `framework/doc/framework/common/spec/server/00-foundation/*.ko.md`, `01-execution/*.ko.md` | `framework/languages/{cpp,dotnet,java,node,kotlin}/**` |
| R5 fw-transport-session | `.../02-channel-transport/*.ko.md`, `.../04-session/*.ko.md` | 같음 + `bindings/doc/spec/README.ko.md`, `core/doc/spec/core/socket/README.ko.md`(하위 보장 대조) |
| R6 fw-spot-actor | `.../03-spot-actor/*.ko.md` | 같음 |
| R7 fw-relocation-observability | `.../05-location-relocation/*.ko.md`, `.../06-observability/*.ko.md` | 같음 |
| R8 fw-language-projections | `.../server/languages/**/*.ko.md`, `.../http-client/**/*.ko.md` | 같음 + 공통 스펙(언어 문서가 공통 규칙을 재정의하는지) |

보고서 경로: `doc/plan/c016-worklog/spec-review/R<n>-<area>-summary.md`.
