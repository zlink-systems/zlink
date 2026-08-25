# ledger-08 — `08-layering.ko.md` 규칙 등가성 대장

대상: `framework/doc/framework/common/spec/server/00-foundation/08-layering.ko.md`
(옛 `40-internal-layering.ko.md`). 매핑표 §3.7·§5(R267~R301) 기준.

| R# | 새 위치 | 비고 |
|---|---|---|
| R267 | §1 첫 문단 | "모든 언어 구현은 같은 책임 그래프를 따른다" 굵은 규칙 + 이유로 재작성 |
| R268 | §1 둘째 문단 | public contract/runtime core/integration 3층, binding type 비노출 |
| R269 | §1 셋째 문단 | 의미·소유권·수명·준비상태·오류 동일 시 직접 호출, 다르면 adapter |
| R270 | §1 넷째 문단 | `SpotNode`/`Stream` 예시, 면제 없음 |
| R271 | §1 bullet("Binding public type은…") | integration 영역 한정, domain contract 복사 금지 |
| R272 | §1 "동작을 분류하는 기준" 표 | 5행 그대로 |
| R273 | §1 표 다음 문단 | adapter 필요 조건 4가지 |
| R274 | §1 "POSDDD 검토 관문" 표 | 6항목 그대로 |
| R275 | §1 "성능 관문" bullet 6개 | 그대로 |
| R276 | §1 "금지하는 구조" bullet 5개 | 그대로 |
| R277 | §1 "언어 간 동일성" bullet | class 이름·file layout 자유, 책임 그래프 등은 동일 |
| R278 | §1 "언어 간 동일성" 마지막 문단 | "정본" → "이 규칙을 지켰는지 판정하는 단일 기준" |
| R279 | §1 "언어별 재량" 문단 | `**언어별 재량**` 표시 + 공통 규칙 + 관찰 결과가 같은 이유 + 확인 기준 명시 |
| R280 | §2 첫 bullet | host runtime 1개, topology runtime은 그 아래, 종료 순서/닫는 방법 소유 분리 |
| R281 | §2 둘째 bullet | 공통 lifecycle 절차(수락 중지→drain→close), idempotent |
| R282 | §2 셋째 bullet + "내부 확인 조건" | 구체 타입 분기 금지(관찰 불가 부분은 내부 확인 조건으로 분리) |
| R283 | §2 "종료 로직을 host 통합 계층에 두지 않는다" | 그대로 |
| R284 | §2 "같은 프로토콜을 두 번 구현하지 않는다" | "정본이 둘이 된다" → "소유하는 곳이 둘이 된다" |
| R285 | §2 "관찰 기준" | 문장은 §7로 옮기고, 여기서는 §7 링크만 남김(가이드 §4.3 — 관찰 가능한 행동은 한 곳에서만 서술) |
| R286 | §3 표 | 이전 후 종료 / 즉시 종료 |
| R287 | §3 첫 bullet | 같은 종류 겹침 → 조건 같으면 합류, 다르면 `Blocked/OperationInProgress` |
| R288 | §3 둘째 bullet | Relocate·Shutdown 겹침 → shutdown 승리, `Blocked/ShutdownRequested` |
| R289 | §3 셋째 bullet + 이어지는 두 문단 | host 전체 사전 검사, target 대기 후 `Blocked/TargetUnavailable`, 거절 결과 미저장 |
| R290 | §3 넷째 문단("옮길 대상이 하나도 없으면…") | 그대로 |
| R291 | §3 다섯째 bullet | 확정 전/후 실패 처리 차이, host 미종료 |
| R292 | §3 여섯째 bullet | 관측 구독자가 진행을 막지 못함 |
| R293 | §4 첫 bullet | resource는 만든 쪽이 닫음, 참조 보관·closed/generation 확인 가능 |
| R294 | §4 둘째 bullet + 9단계 목록 | 9단계 순서, "4번이 5번보다 먼저" 별도 강조 문단 유지 |
| R295 | §4 마지막 bullet | 최종 게시 뒤 신규 callback/timer/이벤트 금지(이유 문장 보강) |
| R296 | §5 두 bullet | 시작 시점 1회 검증·불변, 검증 실패 시 미시작 |
| R297 | §6 표 | 식별자 8종과 유효 범위, 새 diagram(층별 소유)으로 시각화 보강 |
| R298 | §6 "왜 유일성을…" 첫 bullet | `(node RID, lifecycle generation, 호출 식별자)` 조합 |
| R299 | §6 "왜 유일성을…" 둘째 bullet + "일부만 타입을…" bullet + "이름 충돌 주의" | 형식 단일화, 전용 타입·표기 통일, `OperationId` 이름 충돌 회피 |
| R300 | §6 "밖으로 내보내지 않는 값" bullet | 물리 연결 식별자·record version·queue 내부 순번 비공개 |
| R301 | §7 검증 요구 + 각 절의 "내부 확인 조건" | 19항목을 인터페이스 관찰(§7, 11항목)과 내부 확인 조건(§1·§2·§6에 3곳, 8항목)으로 분리 재배치. 항목 손실 없음 |

## 이동 후 갱신할 링크

이 문서가 참조하는 다음 문서는 아직 주제 디렉터리로 옮겨지지 않았다. 이동 시
`../30-host-relocation-flow.ko.md` → `../05-location-relocation/NN-slug.ko.md`,
`../44-internal-relocation-continuity.ko.md` → `../05-location-relocation/NN-slug.ko.md` 형태로
경로와 anchor를 갱신해야 한다(§5 anchor 치환표 작업 대상).

- `../30-host-relocation-flow.ko.md#6-concurrent-호출과-cancellation` (§3, 2회)
- `../30-host-relocation-flow.ko.md#11-shutdown과-relocate의-경쟁` (§3, §4, 2회)
- `../30-host-relocation-flow.ko.md#4-target을-선택하기-전에-확인하는-조건` (§3)
- `../30-host-relocation-flow.ko.md#51-target이-아직-없을-때` (§3, 2회)
- `../30-host-relocation-flow.ko.md#10-relocate-완료와-실패` (§3)
- `../44-internal-relocation-continuity.ko.md#1-네-개의-경계` (§3)

## spec-gap 후보

- **성능 관문의 정량 기준 부재.** §1 "성능 관문"과 §7 검증 요구는 "throughput, p99 latency,
  allocation/GC, lock contention을 기준선과 비교해 측정하고 설명되지 않은 저하가 없어야
  한다"고만 요구하며, 옛 문서(40 §1, §7)에도 구체적인 허용 오차나 기준선 관리 방법이 없다.
  4언어 대조에서 "기준선"을 실제로 어디에 저장·비교하는지 확인이 필요하다. 재작성 범위
  밖이므로 spec-gap 대장에만 기록.
- **R285 "관찰 기준" 문장의 소유 위치.** 옛 문서는 §2 안에 "관찰 기준" 소제목으로 문장을
  직접 담았으나, 이번 재작성은 가이드 §4.3(같은 행동은 한 곳에서만 서술)에 따라 §7로
  옮기고 §2에는 링크만 남겼다. 재작성 오류가 아니라 가이드 적용 결과이므로 4언어 대조에서
  "R285가 §2에 없다"는 보고가 나오면 이 대장을 기준으로 판정할 것.
