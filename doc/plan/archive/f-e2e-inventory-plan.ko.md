# F: e2e_inventory Backlog 착수 계획 (H-11)

작성 2026-08-20 (codex terra high 인벤토리 리서치, Claude 검토·반영). relocation 캠페인
체크리스트 H-11의 근거. "168 backlog"의 실제 = **~185 gate-closure 레코드**(14 config).
분류: A(문서 교차참조 14) · B(feature-map 행 불일치 94) · C(미구현/부분 시나리오 61) · D(source-only 16).

**경계 판정**: Config 6(16)=H-7 소유, Config 10(24: ST-C4=H-1, E/G/H/I 22=H-8)=relocation 캠페인 소유,
**순수 F = A 14 + 나머지 B/C 115 = 129**(+ source-only 16 = 145). ST-F3A는 F 흡수 말고 H 후속 소유자 명시.

**H-8 개수 정정**: 공통 문서 E/G/H/I ID는 정상 ST-E1 제외 시 **22개**(28 아님).

**F 착수 순서**: (1) 인벤토리 정규화(14, doc) → (2) feature-map 행 대조(B 78: Config14→5→2→3→8→11→7)
→ (3) source-only 정합(16) → (4) 실제 E2E authoring(C 37: SubmitAdmission 18→ObservabilityOps 11→
PubSub 5→RuntimeMonitoring 2→ResilienceLifecycle 1) → (5) H 경계 재결정 후 재집계.
1~3은 다수 framework/doc/** feature-map 작업(Claude 소관). 4는 role process·fault seam·evidence 대형 구현.

---

| Config | 공통 ID | B: map ID 없음 | C: 불완전/부분 | H·캠페인 관계 | F B+C 잔여 |
|---|---:|---:|---:|---|---:|
| 1 RegistryMessaging | 16 | 0 | 0 | 없음 | 0 |
| 2 SpotService | 64 | 11 | 0 | 순수 F | 11 |
| 3 PubSub | 24 | 10 | 5 | 순수 F | 15 |
| 4 RegistrationCodec | 10 | 0 | 0 | 없음 | 0 |
| 5 ResilienceLifecycle | 35 | 17 | 1 | 순수 F | 18 |
| 6 DiscoveryRegistryHa | 28 | 14 | 2 | H-7 및 relocation Track F가 이미 소유 | 0 |
| 7 RuntimeMonitoring | 13 | 1 | 2 | 순수 F | 3 |
| 8 AutomaticTurnDispatch | 30 | 5 | 0 | 순수 F | 5 |
| 9 ToActorMessaging | 6 | 0 | 0 | 없음 | 0 |
| 10 SpotActorTransfer | 44 | 2 | 22 | H-1/H-8 및 별도 orphan 1건 | 0 |
| 11 ObservabilityOps | 21 | 3 | 11 | 명시 H 소유 없음 | 14 |
| 12 ChannelEgressRouting | 16 | 0 | 0 | 없음 | 0 |
| 13 SubmitAdmission | 19 | 0 | 18 | 순수 F | 18 |
| 14 InstanceSpot | 35 | 31 | 0 | 순수 F | 31 |
| **합계** | **361** | **94** | **61** |  | **115** |

따라서 정확한 현재 의미는 다음과 같습니다.

| 분류 | 장부 기준 | 실제 전수 기준 | 내용 |
|---|---:|---:|---|
| A. 문서 교차참조 | 14 | 14 | config 문서 14쌍이 구체적인 C++ feature-map으로 연결되지 않음 |
| B. feature-map 행 불일치 | 94 | 94 | 공통 문서 ID가 C++ feature-map에 전혀 없음 |
| C. 미구현/부분 scenario | 60 | 61 | gate 60 + 누락된 `ST-H4 partial` 1 |
| D. 기타 | 0 | 16 추가 | source/runner ID 누락 중 A/B/C와 겹치지 않는 항목 |
| **합계** | **168** | **185 gate-closure 레코드** | 169 문서/상태 레코드 + source-only 16 |

B/C의 대표 및 정확한 ID 묶음은 다음과 같습니다.

- Config 2 B: `SM-A9~A13`, `SM-B0`, `SM-B0A`, `SM-B10~B11`, `SM-G5A~G5B`
- Config 3 B: `PS-D7A/B`, `PS-E2A~E2C`, `PS-F1~F5`; C: `PS-D3~D6`, `PS-E1`
- Config 5 B: `RL-E1~E5`, `RL-F1`, `RL-F3`, `RL-F5~F14`; C: `RL-D5`
- Config 6 B: `SF-B3`, `SF-C3~C5A`, `SF-F1`, `SF-F4~F6`, `SF-F8~F10`, `SF-G1~G2`; C: `SF-F7`, `SF-F11`
- Config 7 B: `MON-A7`; C: `MON-B1~B2`
- Config 8 B: `TD-D4~D6`, `TD-E2A`, `TD-F5A`
- Config 10 B: `ST-E1B/C`; C: `ST-C4`, `ST-E1A`, `ST-F3A`, `ST-G1~G6`, `ST-H1~H5`, `ST-H4A/B`, `ST-I1~I6`
- Config 11 B: `OBS-A5`, `OBS-C9A`, `OBS-C12`; C: `OBS-B1`, `OBS-C1~C8`, `OBS-C10~C11`
- Config 13 C: `SA-E2E-01~09`, `11~13`, `15~20`
- Config 14 B: `IS-E2E-01`, `04`, `06~08`, `10~16`, `18~36`

추가 D 16건은 source ID만 빠진 경우입니다.

- Config 2: `SM-C6`, `SM-D4A`, `SM-D4B`, `SM-D5A`
- Config 3: `PS-D1`, `PS-D2`
- Config 8: `TD-A5`, `TD-C4`, `TD-C5`, `TD-F1~F6`, `TD-G1`

## relocation 캠페인과 F의 경계

캠페인 계획은 F를 “168건, 14개 문서”로 적지만, 현재 H 항목과 실질적으로 겹칩니다. [`relocation-campaign-checklist.ko.md:833`](relocation-campaign-checklist.ko.md#e-확정-후속-단계-사용자-승격-2026-08-19--완료-조건-포함-c-완료-후-착수)

- **Config 6의 16건**은 H-7의 “미구현 14 SF”와 정확히 맞고, `SF-F7/F11`은 캠페인 Track F에서 이미 손댄 뒤 의도적으로 남은 하위 variant입니다. H-7 근거는 [`계획:969`](relocation-campaign-checklist.ko.md#h-후속-트랙-전면-착수-사용자-지시-2026-08-20--후속으로-표시된-것도-리스트업하고-모두-진행)입니다.
- **Config 10의 24건**은 relocation 전용입니다.
  - `ST-C4`는 H-1 소유입니다. [`계획:934`](relocation-campaign-checklist.ko.md#h-후속-트랙-전면-착수-사용자-지시-2026-08-20--후속으로-표시된-것도-리스트업하고-모두-진행)
  - E/G/H/I 묶음 22개는 H-8과 대응합니다.
  - `ST-F3A`는 현재 H-1/H-8 어느 쪽에도 명시 배정되지 않은 relocation-adjacent orphan입니다.
- H-8은 “28개”라고 쓰지만, 공통 문서의 E/G/H/I ID는 정상 `ST-E1`을 빼면 **22개**입니다. 6개 차이와 `ST-F3A`의 소유권은 F 착수 전에 정정해야 합니다.
- Config 11의 C 11개는 Host Relocate/Shutdown 성격이 강하지만 현재 H 소유 표시는 없습니다. 따라서 “relocation과 무관”이라는 F 설명은 기능 의미상 정확하지 않고, 여기서는 **캠페인 ledger에 아직 배정되지 않았다는 뜻**으로만 해석해야 합니다.

권장 경계는 다음입니다.

- H/캠페인 계속 소유: Config 6의 16, Config 10의 24
- 순수 F 문서·map·상태: A 14 + 나머지 B/C 115 = **129**
- F가 실제 inventory gate closure도 목표로 하면 source-only 16을 더해 **145**
- `ST-F3A`는 F에 흡수하지 말고 H 후속 소유자로 명시

## F 착수 순서와 규모

1. **인벤토리 정규화 — 14 records, 28 protected config files**

   각 EN/KO config 문서에서 해당 언어 feature-map을 명시 링크하고, `ST-H4 partial`을 포함하도록 inventory 정규식을 고칩니다. 이 단계가 먼저여야 이후 수치가 흔들리지 않습니다.

2. **대형 feature-map 행 대조 — B 78 records, 문서 중심**

   Config 14(31) → Config 5(17) → Config 2(11) → Config 3(10) → Config 8(5) → Config 11(3) → Config 7(1) 순서입니다. 먼저 실제 selector/source 존재 여부를 판정하고, 존재하면 map을 정정하며, 없으면 C 작업으로 승격합니다.

3. **source-only 정합성 — 16 records, 소규모 증거 보강**

   Config 2·3·8의 runner/selector가 ID를 실제로 표현하는지 확인합니다. 이들은 feature-map을 추가해도 gate가 남을 수 있는 항목이므로 2단계와 병행하면 좋습니다.

4. **실제 E2E authoring — C 37 records, 기능군별 대형 작업**

   Config 13 SubmitAdmission(18) → Config 11 ObservabilityOps(11) → Config 3 PubSub(5) → Config 7 RuntimeMonitoring(2) → Config 5 ResilienceLifecycle(1) 순입니다. 이 묶음은 문서 정리가 아니라 role process, fault seam, public evidence가 필요한 구현 작업입니다.

5. **H 경계 재결정 후 재집계**

   H-7/H-8/H-1 및 `ST-F3A`를 F와 중복 없이 배정한 뒤에만 전체 `test_cpp_framework_common_e2e_inventory`를 다시 기준선으로 삼는 것이 맞습니다. script는 feature-map/소스 정합성 gate일 뿐 실제 process E2E 성공 증거는 아닙니다.


tokens used
100,461
## 결론

`168`은 현재 gate의 모든 실패를 뜻하지 않는 장부 수입니다.

- 장부상: 교차참조 14 + feature-map ID 누락 94 + gate가 잡은 불완전 상태 60 = **168**
- 실제 상태 표기를 전수로 읽으면 `ST-H4 | partial`을 gate 정규식이 놓쳐 **169건**
- source/runner ID 검사에는 위와 겹치지 않는 **16건**이 더 있어, F가 gate까지 닫으려면 순수 F 잔여는 **129 + 16 = 145개 레코드**로 잡는 것이 안전합니다.

변경은 하지 않았습니다.

## Gate 및 현재 결과

CTest 등록은 [`CMakeLists.txt`](../../../framework/languages/cpp/CMakeLists.txt#L1284)의 `test_cpp_framework_common_e2e_inventory`이며, `framework-contract;framework-e2e` 레이블입니다. 실제 검사는 [`verify_common_inventory.sh`](../../../framework/languages/cpp/e2e/verify_common_inventory.sh#L57)에서 다음을 확인합니다.

- 14개 공통 config의 EN/KO scenario-ID parity
- C++ feature-map의 ID 존재
- `미구현|부분|blocked|deferred|component only|not-supported` 상태
- runner/source에 해당 ID의 참조 존재

현 HEAD에서 직접 실행 결과는 공통 시나리오 **361개**, feature-map ID 누락 **94**, source 누락 **122**, 불완전 상태 **60**, predicate 실패 합계 **276**입니다. EN/KO pair 및 ID parity 오류는 없었습니다.

중요한 gate 결함도 하나 있습니다. 정규식이 영어 상태값 `partial`을 검사하지 않아 [`SpotActorTransfer/feature-map.ko.md:47`](../../../framework/languages/cpp/e2e/SpotActorTransfer/feature-map.ko.md#c-spotactortransfer-e2e-feature-map)의 `ST-H4`를 누락합니다. 따라서 “168”은 완전한 상태 인벤토리가 아닙니다.

## 14-config 상세 인벤토리

`A`는 각 config 문서 쌍에 concrete language feature-map 링크가 없는 교차참조 1건입니다. 14개 `config-*.ko/en.md` 모두 해당합니다. `B/C`는 아래 ID 기준이며, `F 잔여`는 H 트랙 소유 Config 6/10을 제외한 수입니다.

| Config | 공통 ID | B: map ID 없음 | C: 불완전/부분 | H·캠페인 관계 | F B+C 잔여 |
|---|---:|---:|---:|---|---:|
| 1 RegistryMessaging | 16 | 0 | 0 | 없음 | 0 |
| 2 SpotService | 64 | 11 | 0 | 순수 F | 11 |
| 3 PubSub | 24 | 10 | 5 | 순수 F | 15 |
| 4 RegistrationCodec | 10 | 0 | 0 | 없음 | 0 |
| 5 ResilienceLifecycle | 35 | 17 | 1 | 순수 F | 18 |
| 6 DiscoveryRegistryHa | 28 | 14 | 2 | H-7 및 relocation Track F가 이미 소유 | 0 |
| 7 RuntimeMonitoring | 13 | 1 | 2 | 순수 F | 3 |
| 8 AutomaticTurnDispatch | 30 | 5 | 0 | 순수 F | 5 |
| 9 ToActorMessaging | 6 | 0 | 0 | 없음 | 0 |
| 10 SpotActorTransfer | 44 | 2 | 22 | H-1/H-8 및 별도 orphan 1건 | 0 |
| 11 ObservabilityOps | 21 | 3 | 11 | 명시 H 소유 없음 | 14 |
| 12 ChannelEgressRouting | 16 | 0 | 0 | 없음 | 0 |
| 13 SubmitAdmission | 19 | 0 | 18 | 순수 F | 18 |
| 14 InstanceSpot | 35 | 31 | 0 | 순수 F | 31 |
| **합계** | **361** | **94** | **61** |  | **115** |

따라서 정확한 현재 의미는 다음과 같습니다.

| 분류 | 장부 기준 | 실제 전수 기준 | 내용 |
|---|---:|---:|---|
| A. 문서 교차참조 | 14 | 14 | config 문서 14쌍이 구체적인 C++ feature-map으로 연결되지 않음 |
| B. feature-map 행 불일치 | 94 | 94 | 공통 문서 ID가 C++ feature-map에 전혀 없음 |
| C. 미구현/부분 scenario | 60 | 61 | gate 60 + 누락된 `ST-H4 partial` 1 |
| D. 기타 | 0 | 16 추가 | source/runner ID 누락 중 A/B/C와 겹치지 않는 항목 |
| **합계** | **168** | **185 gate-closure 레코드** | 169 문서/상태 레코드 + source-only 16 |

B/C의 대표 및 정확한 ID 묶음은 다음과 같습니다.

- Config 2 B: `SM-A9~A13`, `SM-B0`, `SM-B0A`, `SM-B10~B11`, `SM-G5A~G5B`
- Config 3 B: `PS-D7A/B`, `PS-E2A~E2C`, `PS-F1~F5`; C: `PS-D3~D6`, `PS-E1`
- Config 5 B: `RL-E1~E5`, `RL-F1`, `RL-F3`, `RL-F5~F14`; C: `RL-D5`
- Config 6 B: `SF-B3`, `SF-C3~C5A`, `SF-F1`, `SF-F4~F6`, `SF-F8~F10`, `SF-G1~G2`; C: `SF-F7`, `SF-F11`
- Config 7 B: `MON-A7`; C: `MON-B1~B2`
- Config 8 B: `TD-D4~D6`, `TD-E2A`, `TD-F5A`
- Config 10 B: `ST-E1B/C`; C: `ST-C4`, `ST-E1A`, `ST-F3A`, `ST-G1~G6`, `ST-H1~H5`, `ST-H4A/B`, `ST-I1~I6`
- Config 11 B: `OBS-A5`, `OBS-C9A`, `OBS-C12`; C: `OBS-B1`, `OBS-C1~C8`, `OBS-C10~C11`
- Config 13 C: `SA-E2E-01~09`, `11~13`, `15~20`
- Config 14 B: `IS-E2E-01`, `04`, `06~08`, `10~16`, `18~36`

추가 D 16건은 source ID만 빠진 경우입니다.

- Config 2: `SM-C6`, `SM-D4A`, `SM-D4B`, `SM-D5A`
- Config 3: `PS-D1`, `PS-D2`
- Config 8: `TD-A5`, `TD-C4`, `TD-C5`, `TD-F1~F6`, `TD-G1`

## relocation 캠페인과 F의 경계

캠페인 계획은 F를 “168건, 14개 문서”로 적지만, 현재 H 항목과 실질적으로 겹칩니다. [`relocation-campaign-checklist.ko.md:833`](relocation-campaign-checklist.ko.md#e-확정-후속-단계-사용자-승격-2026-08-19--완료-조건-포함-c-완료-후-착수)

- **Config 6의 16건**은 H-7의 “미구현 14 SF”와 정확히 맞고, `SF-F7/F11`은 캠페인 Track F에서 이미 손댄 뒤 의도적으로 남은 하위 variant입니다. H-7 근거는 [`계획:969`](relocation-campaign-checklist.ko.md#h-후속-트랙-전면-착수-사용자-지시-2026-08-20--후속으로-표시된-것도-리스트업하고-모두-진행)입니다.
- **Config 10의 24건**은 relocation 전용입니다.
  - `ST-C4`는 H-1 소유입니다. [`계획:934`](relocation-campaign-checklist.ko.md#h-후속-트랙-전면-착수-사용자-지시-2026-08-20--후속으로-표시된-것도-리스트업하고-모두-진행)
  - E/G/H/I 묶음 22개는 H-8과 대응합니다.
  - `ST-F3A`는 현재 H-1/H-8 어느 쪽에도 명시 배정되지 않은 relocation-adjacent orphan입니다.
- H-8은 “28개”라고 쓰지만, 공통 문서의 E/G/H/I ID는 정상 `ST-E1`을 빼면 **22개**입니다. 6개 차이와 `ST-F3A`의 소유권은 F 착수 전에 정정해야 합니다.
- Config 11의 C 11개는 Host Relocate/Shutdown 성격이 강하지만 현재 H 소유 표시는 없습니다. 따라서 “relocation과 무관”이라는 F 설명은 기능 의미상 정확하지 않고, 여기서는 **캠페인 ledger에 아직 배정되지 않았다는 뜻**으로만 해석해야 합니다.

권장 경계는 다음입니다.

- H/캠페인 계속 소유: Config 6의 16, Config 10의 24
- 순수 F 문서·map·상태: A 14 + 나머지 B/C 115 = **129**
- F가 실제 inventory gate closure도 목표로 하면 source-only 16을 더해 **145**
- `ST-F3A`는 F에 흡수하지 말고 H 후속 소유자로 명시

## F 착수 순서와 규모

1. **인벤토리 정규화 — 14 records, 28 protected config files**

   각 EN/KO config 문서에서 해당 언어 feature-map을 명시 링크하고, `ST-H4 partial`을 포함하도록 inventory 정규식을 고칩니다. 이 단계가 먼저여야 이후 수치가 흔들리지 않습니다.

2. **대형 feature-map 행 대조 — B 78 records, 문서 중심**

   Config 14(31) → Config 5(17) → Config 2(11) → Config 3(10) → Config 8(5) → Config 11(3) → Config 7(1) 순서입니다. 먼저 실제 selector/source 존재 여부를 판정하고, 존재하면 map을 정정하며, 없으면 C 작업으로 승격합니다.

3. **source-only 정합성 — 16 records, 소규모 증거 보강**

   Config 2·3·8의 runner/selector가 ID를 실제로 표현하는지 확인합니다. 이들은 feature-map을 추가해도 gate가 남을 수 있는 항목이므로 2단계와 병행하면 좋습니다.

4. **실제 E2E authoring — C 37 records, 기능군별 대형 작업**

   Config 13 SubmitAdmission(18) → Config 11 ObservabilityOps(11) → Config 3 PubSub(5) → Config 7 RuntimeMonitoring(2) → Config 5 ResilienceLifecycle(1) 순입니다. 이 묶음은 문서 정리가 아니라 role process, fault seam, public evidence가 필요한 구현 작업입니다.

5. **H 경계 재결정 후 재집계**

   H-7/H-8/H-1 및 `ST-F3A`를 F와 중복 없이 배정한 뒤에만 전체 `test_cpp_framework_common_e2e_inventory`를 다시 기준선으로 삼는 것이 맞습니다. script는 feature-map/소스 정합성 gate일 뿐 실제 process E2E 성공 증거는 아닙니다.


