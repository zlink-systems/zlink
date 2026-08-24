# Stage-8 D4 최종 보고 매트릭스 초안

기준: Stage-7 보고서와 `git log --oneline -40`에서 확인한 사실만 기록했다. `미확인`은
추정으로 채우지 않는다. Java Framework는 Kotlin 샘플이 같은 JVM 구현을 공유하므로 표에서
Java로 묶는다.

## 1. Canonical 마이그레이션

| 언어 | 수신 | 발신 | attempt-lifecycle | 사설 dialect 제거 | B1 | B2 | B3 | B4 |
|---|---|---|---|---|---|---|---|---|
| Node | 완료; 12셀 실경로 근거 `fe352b9e87` | 완료; 12셀 실경로 근거 `fe352b9e87` | 완료; later-attempt-wins 실증 `327c2b86c1` | 완료 `694d849a7e` | 완료; 로드맵상 기존, 개별 커밋 미확인 | 완료 `c159b59413` | 완료 `669ffa6735` | 완료 `b996d6b7df` |
| .NET | 완료 `e8409034c6` | 완료 `b67385822e` | 완료; later-attempt-wins 실증 `327c2b86c1` | 완료 `9ec9ede6c0` | 완료 `7bf39913e5` | 완료 `720f709d35` | 완료 `bb191b0be4` | 완료 `bb191b0be4` |
| Java | 완료; 12셀 실경로 근거 `fe352b9e87` | 완료; 로드맵의 이미 랜딩된 기반, 개별 커밋 미확인 | 완료; later-attempt-wins 실증 `327c2b86c1` | 완료 `aee042f05b` | 완료 `02e0126ecb` | 완료 `618986be28` | 완료 `812b5cc390` | 완료 `812b5cc390` |
| C++ | 완료 `5f22587b0b` | 완료 `7ca95170ac` | 완료; later-attempt-wins 실증 `327c2b86c1` | 완료 `277a3ede16` | 완료 `f9515f0277` | 완료 `e354685afc` | 완료 `d6307e3130` | 완료 `d6307e3130` |

사설 dialect 제거의 단계 DoD는 4언어 코드 제거와 fresh host 재빌드를 포함한 12셀 2연속
그린으로 완료 처리됐다. 수신·발신의 `fe352b9e87` 근거는 12방향 canonical harness 편입이다.

## 2. 7샘플 × 4언어 게이트

표의 HEAD는 해당 보고서가 명시한 게이트 시점 HEAD만 적었다. 보고서에 HEAD가 없으면
`미확인`이다.

| 샘플 | Node (HEAD) | .NET (HEAD) | Java (HEAD) | C++ (HEAD) |
|---|---|---|---|---|
| Bingo | 2/2 그린 (`미확인`) | 2/2 그린 (`미확인`) | 2/2 그린 (`5a4ed6f520`) | 2/2 그린 (`d6307e3130`) |
| DeliveryDispatch | 2/2 그린 (`미확인`) | 2/2 그린 (`미확인`) | 2/2 그린 (`5a4ed6f520`) | 2/2 그린 (`d6307e3130`) |
| GameQuest | 2/2 그린 (`미확인`) | 2/2 그린 (`미확인`) | 2/2 그린 (`5a4ed6f520`) | 2/2 그린 (`d6307e3130`) |
| ShoppingMall | 2/2 그린 (`미확인`) | 2/2 그린 (`미확인`) | 2/2 그린 (`5a4ed6f520`) | 2/2 그린 (`d6307e3130`) |
| SupportChat | 2/2 그린 (`미확인`) | 2/2 그린 (`미확인`) | 2/2 그린 (`5a4ed6f520`) | 2/2 그린 (`d6307e3130`) |
| TicTacToe | 2/2 그린 (`미확인`) | 2/2 그린 (`미확인`) | 2/2 그린 (`5a4ed6f520`) | 정책 8/8 통과 (`d6307e3130`) |
| ZoneWorld | **수정 검증 중**: 기존 gate는 1회 통과 뒤 ZW-B8 실패; 최종 gate HEAD 미확인 | 2/2 그린 (`미확인`) | 2/2 그린 (`5a4ed6f520`); Kotlin ZoneWorld도 2/2 | 2/2 그린 (`d6307e3130`) |

Node의 여섯 비-ZoneWorld 셀은 `report-node-7gate-2.md`의 2/2 결과다. ZoneWorld는 같은
보고서가 두 번째 기본 full lane의 ZW-B8 실패로 중단됐다고 기록하므로, 별도의 후속 수정 보고가
있더라도 이 최종 매트릭스 초안에서는 재게이트 HEAD와 2연속 증거가 명시될 때까지 완료로
표기하지 않는다.

## 3. ZoneWorld 정본과 golden

| 항목 | 상태 | 근거 |
|---|---|---|
| 시나리오 정본 | v2, 34 ID | 4언어 ZoneWorld runner의 named-ID 판정 모델 |
| golden 패키지 | v1, 9세트·정규화 78레코드 | `f5ed56e4e8`; clean .NET reference lane, manifest SHA-256 및 raw artifact 검증 완료 |
| .NET 완주 | 2/2 full lane | `report-dotnet-7gate-3.md` |
| Java/Kotlin 완주 | Java 7샘플 및 Kotlin ZoneWorld 각 2/2 | `report-java-7gate-3.md` |
| C++ 완주 | 2/2 full lane | `report-cpp-7gate.md`, `d6307e3130` |
| Node 완주 | 미확인 | ZW-B8 수정 검증 중; 위 §2의 기존 실패 근거 유지 |

## 4. D3 게이트

| 게이트 | 상태 | HEAD/근거 |
|---|---|---|
| JVM 집계 `check --continue` | 그린 | `895d7a6963`; 72 actionable tasks, 최종 실패 subproject 없음 |
| JVM doc examples build | 그린 | `895d7a6963`; `:zlink-framework-doc-examples:build` |
| harness `all` 스윕 | 예정 | 실행 결과 미확인; 실행 시 HEAD·명령·결과를 이 칸에 기록 |

## 5. 오늘 확인·수정된 Framework 결함

| 언어 | 원인과 수정 | 커밋 |
|---|---|---|
| Node | deferred Actor Join 완료 turn이 provisional backlog 취소 replay를 기다려 Spot serial turn이 순환 대기 | `f401c0181a` |
| Node | dispatch pump의 detached AsyncLocal snapshot이 false same-owner nesting을 만들 수 있음 | `6c00e87237` |
| .NET | disposed parent runtime에서 fanout subscriber loop dispose가 예외를 냄 | `18ec5f8248` |
| .NET | supersession identity를 pre-gate에서 걸러 B8 proof 전에 잘못 진행 | `9e1bf0d945` |
| .NET | precommit lease heartbeat 충돌을 application exception으로 표면화 | `ed0bb8e7c0` |
| Java | Instance Spot close가 두 lane의 quiescence 전에 `onClosing`으로 진행 | `5a4ed6f520` |
| Java/Kotlin | B8에서 replacement 단언보다 fault proof가 뒤서던 순서 | `c13302a8a5` |
| C++ | control-plane 및 ZLJR generated codec 소유권이 분산됨 | `d6307e3130` |

## 6. Clean-break 원칙

사설 actor-join dialect 제거가 완료됐으므로 구버전 wire와의 호환은 제공하지 않는다. 실행마다
Redis 상태를 drain하고 스토어를 초기화한다. 기존 상태를 읽는 호환 분기, 수동 row 덮어쓰기,
구버전 wire fallback으로 이전 실행의 상태를 재사용하지 않는다.

## 7. 잔여·이월 카드

| 카드 | 현재 기록 |
|---|---|
| canonical wire-admission 음성 검사 | Authority-row fence mismatch, TypeMismatch, malformed→ProtocolError, mailbox-full unit은 미완료 |
| C++ throwing reply serializer | pending-admission unwind pathological 경로 미커버 |
| spec 51 §9 reply 조항 | 오류 수정 판정 완료, 문서 잠금 해제 대기 |
| .NET 3c revert-target | nested-envelope 호환 분기와 `RecoveryReplyContentType` 단순화 재검토 |
| durable legacy JSON recovery | 이중 표현 제거의 마이그레이션 판정 필요 |
| 28 reply content type | 4언어 보존 조항 충족성 교차 검증 필요 |
| D3 harness `all` | 예정; 결과 미확인 |
| 단계 9 B1/B2/B3 | 모든 E2E authoring·dispatch 연결·4언어 lockstep green·spec-gap clean이 남음 |
| 단계 10 D5 | 최종 sign-off 미착수 |
