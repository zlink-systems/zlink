# 작업 백로그 (진행·대기 전체 목록)

> 갱신: 2026-08-30. 소유: Claude 감독관 세션 — 트랙 상태가 바뀔 때마다 이 문서를 갱신한다.
> 작업자 정책: codex terra/sol **medium**(기본) + sonnet/opus(Claude) 병용. sol ultra 사용 중지(2026-08-30).
> 큰 흐름의 진행표는 `implementation-plan.ko.md` §0.6, 이 문서는 실행 단위 백로그.

## A. 진행 중 (2026-08-30 시점)

| # | 트랙 | 작업자 | 내용 | 완료 조건 |
|---|---|---|---|---|
| A1 | cpp B-2 TOCTOU fence | opus | `relocation-toctou-verdict.ko.md` 판정 구현 — coordinator admission↔Actor FIFO admission 원자화(membership :903 위반 해소). 잠재 결함 수정 | B-2 인터리빙 회귀 + 게이트 45/45 + ZW ×3 |
| A2 | java B-2 silent drop | opus | post-cut queue 거부를 호출자가 묵살하는 유실 경로 수정 — ingress hold/capture 회송(routing :222) | 회귀 + gradle 게이트 + ZW java ×3·kotlin ×1 |
| A3 | dotnet registry 재조회 제거 | sol medium | 08-routing §2.6(:316) 위반 과잉 검증 삭제(-2회, 14/15→12/13) + reset clear→fence 창의 same-generation 재게시 가능성 실증(가능하면 :471 위반으로 수정) | Deferred Join 캡처 계약(:6576) 보존 + 전체 게이트 + ZW ×3 |
| A4 | 과잉 검증 전수 감사 ×4 | sol medium ×4 | 언어별 판정 지점 전수 분류(정당 10 / 금지 A·B·C / [의심]) — 1차 시도는 codex 한도 소진으로 유실, 재실행 중 | 언어별 분류표 + 과잉 우선순위 |
| A5 | 동적 제어 표면 dual 정렬 | sol medium | 스펙 06 §5 신설 조항(dual: async 정본 + sync 최소 bridge, sync는 framework 문맥 밖 전용) 구현 — 4언어 인벤토리 후 비파괴 추가 | 언어별 게이트 + 신규 표면 단위 테스트 |

## B. 대기 (선행 조건 있음)

| # | 트랙 | 선행 | 내용 |
|---|---|---|---|
| B1 | 과잉 검증 일괄 제거 라운드 | A4 | 감사 "과잉" 판정분을 언어별 제거 — 각 게이트+ZW 재실증 |
| B2 | cpp 7번째 wait 재설계 | A1 | B-2 fence가 자리 잡으면 warm materialization 재검증의 존치/이관 재판정(send 7→6 가능성) — 단독 판단 금지, fence 설계와 함께 |
| B3 | 감사 [의심] 미분류 정리 | A4·B1 | dotnet 386곳·cpp 662곳 콜드/핫 정밀 분류(선택적 — B1이 상당 부분 흡수 예상) |

## C. 외부/판정 대기

| # | 트랙 | 대기 사유 |
|---|---|---|
| C1 | bindings 0.14.x uplift 정례화 | core 성능·버그 작업 진행 중(0.14.6 반영 완료 상태). 다음 버전 확정 시: 로컬 패키지 빌드 → 4언어 게이트 → ZW 스모크. **주의**: 0.14.5는 Linux x64 릴리스 CI 실패로 아티팩트 404(0.14.6에서 해소) — 릴리스 파이프라인 실패 시 cpp 샘플은 provenance 고정 때문에 전면 차단됨 |
| C2 | dotnet ZW 간헐 (~1/3) | D1·G3-restart·F4 산발, 0.14.1/0.14.2 교차로 버전 무관 확인. 발현 지속 시 진단 라운드(증거: scratchpad zw-evidence/) |
| C3 | cpp channel_messaging flake | 관찰 지속(최근 연속 통과) |

## D. 종결 기록 (이번 주기)

- ZoneWorld 4언어 종결 — tag `seven-samples-green-v1` (4bad5ac979)
- core/bindings 0.14.2 감사 이슈 없음 — tag `core-bindings-0.14.2-audited`; 이후 0.14.6 로컬 반영·cpp 게이트 45/45
- lock 경계 회수 캠페인 — tag `lock-recovery-campaign-v1`: 핫패스 cpp 17/19→**7/7**, dotnet 19/20→**14/15**, java hot wrapper 7→**3** (`lock-boundary-survey.ko.md`)
- 스펙 상세화 2건: binding §5 bind 승인=3종 값(f51ed3757a) · 06 §5 동적 제어 표면 dual(2026-08-30)
- 4언어 bind lease 발산 종결(a0f48c9941) · GameQuest RecreateOnRelocation 통일(844a407407)
- TOCTOU 판정 완결 — `relocation-toctou-verdict.ko.md` (cpp·java B-2 실존 → A1·A2)

## 판정 원칙 (요약 링크)

- generation 비교 허용 10지점 / 금지 3조항(§2.6 전달·§5 bind lease·§8.1 파생 사본) — overval 감사 룰북, 상세는 08-routing §2.6·binding §5·§8.1
- 새 제어점·기계장치 신설 금지, 통합·삭제 우선 (zoneworld-simplicity-discipline)
- unit 그린≠샘플 그린 — 샘플 실증은 감독관 또는 에이전트 직접 실행으로 완료 조건화
