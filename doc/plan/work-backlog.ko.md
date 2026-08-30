# 작업 백로그 (진행·대기 전체 목록)

> 갱신: 2026-08-30. 소유: Claude 감독관 세션 — 트랙 상태가 바뀔 때마다 이 문서를 갱신한다.
> 작업자 정책: codex terra/sol **medium**(기본) + sonnet/opus(Claude) 병용. sol ultra 사용 중지(2026-08-30).
> 큰 흐름의 진행표는 `implementation-plan.ko.md` §0.6, 이 문서는 실행 단위 백로그.

## A. 진행 중 (2026-08-30 시점)

| # | 트랙 | 작업자 | 내용 | 완료 조건 |
|---|---|---|---|---|
| ~~A1~~ | ~~cpp B-2 TOCTOU fence~~ | **완료** `99ed45f887` | `relocation-toctou-verdict.ko.md` 판정 구현 — coordinator admission↔Actor FIFO admission 원자화(membership :903 위반 해소). 잠재 결함 수정 | B-2 인터리빙 회귀 + 게이트 45/45 + ZW ×3 |
| ~~A2~~ | ~~java B-2 silent drop~~ | **완료** `cc750dba3d` | post-cut queue 거부를 호출자가 묵살하는 유실 경로 수정 — ingress hold/capture 회송(routing :222) | 회귀 + gradle 게이트 + ZW java ×3·kotlin ×1 |
| ~~A3~~ | ~~dotnet registry 재조회 제거~~ | **완료** `19181631c8`(14/15→12/13, reset 창 실결함 수정 포함) | 08-routing §2.6(:316) 위반 과잉 검증 삭제(-2회, 14/15→12/13) + reset clear→fence 창의 same-generation 재게시 가능성 실증(가능하면 :471 위반으로 수정) | Deferred Join 캡처 계약(:6576) 보존 + 전체 게이트 + ZW ×3 |
| ~~A4~~ | ~~과잉 검증 전수 감사 ×4~~ | — | **완료(2026-08-30)** — `overvalidation-audit.ko.md`: 과잉 합계 ~51지점(cpp 14·dotnet 22·java 10계열·node 5) → B1 승격 |
| ~~A5~~ | ~~동적 제어 표면 dual 정렬~~ | **완료** `f6fd8ba77a`(8표면 dual) | 스펙 06 §5 신설 조항(dual: async 정본 + sync 최소 bridge, sync는 framework 문맥 밖 전용) 구현 — 4언어 인벤토리 후 비파괴 추가 | 언어별 게이트 + 신규 표면 단위 테스트 |

## B. 대기 (선행 조건 있음)

| # | 트랙 | 선행 | 내용 |
|---|---|---|---|
| ~~B1~~ | ~~과잉 검증 일괄 제거~~ | **완료** | node `b38b0fd66e`(4) · java `3f4fb099ba`(10계열) · cpp `4a26163ff9`(14 — C4 정산 복원 재구현) · dotnet `4c8036a494`(21 — 초과 변경 3건 이분 수정). 합계 ~49곳 제거, 전 언어 게이트+ZW 재실증 그린 |
| ~~B2~~ | ~~cpp 7번째 wait~~ | **완료** `4e7839149b` | warm projection을 admission token turn으로 이관 — **send/request 7→6**(보장 승계표 완비), 게이트 45/45·ZW 5/5 |
| ~~B3~~ | ~~감사 [의심] 미분류 정리~~ | **종결 판정(2026-08-30 감독관)** | 판정 경로의 과잉은 A4 감사가 전수 분류·B1이 제거 완료. 잔여 미분류는 §5가 정당화하는 콜드 bridge 인벤토리로 기능·성능 실익 없음 — 정밀 분류는 향후 성능 감사 필요 시 재개 |
| ~~B4~~ | ~~java MF 수렴 구현~~ | **완료** `bfd4fe44e1`(rebase 후 dabdaf98cc로 push) — ZLinkActorTransferHandoff 단일 owner, raw forward 축 제거, 완료 조건 8/8, ZW java 5/5·kotlin 2/2. **4언어 통합 MF 모델 수렴 종결** | A2 발견[H]: java 프로덕션은 소스측 relocation 이후 전달을 forward+retention 타이머에만 의존(MF API는 테스트 전용) — dotnet(_sourceHoldFrames+MF 전환)과 **언어 발산**. 발산→스펙 상세화 규율 대상 |
| ~~B5~~ | ~~java dispatch 실패 묵살~~ | **완료** `8c77bbfa01` — 전 실패를 원인 kind 보존 terminal로 표면화 |

## C. 외부/판정 대기

| # | 트랙 | 대기 사유 |
|---|---|---|
| C1 | bindings 0.14.x uplift 정례화 | core 성능·버그 작업 진행 중(0.14.6 반영 완료 상태). 다음 버전 확정 시: 로컬 패키지 빌드 → 4언어 게이트 → ZW 스모크. **주의**: 0.14.5는 Linux x64 릴리스 CI 실패로 아티팩트 404(0.14.6에서 해소) — 릴리스 파이프라인 실패 시 cpp 샘플은 provenance 고정 때문에 전면 차단됨 |
| C2 | dotnet ZW 간헐 (~1/3) | D1·G3-restart·F4 산발, 0.14.1/0.14.2 교차로 버전 무관 확인. 발현 지속 시 진단 라운드(증거: scratchpad zw-evidence/) |
| C3 | cpp channel_messaging flake | 관찰 지속(최근 연속 통과) |

## D. 종결 기록 (이번 주기)

**백로그 A·B 전 트랙 완주(2026-08-30)** — 핫패스 최종: cpp **6/6**(시작 17/19), dotnet **12/13**(시작 19/20), java hot wrapper **3**(시작 7). 과잉 검증 ~49곳 제거(4언어), 잠재 결함 처치: cpp B-2 fence·java post-cut drop·java 실패 묵살·dotnet reset 창·java MF 계약 결함 5종. C1~C3만 외부/관찰 대기로 존속.

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
