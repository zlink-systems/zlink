# Framework common E2E scenario 중복·경계 정리안

> 상태: `16 MERGES APPLIED / COMMON INVENTORY VALIDATED / LANGUAGE IMPLEMENTATION NOT UPDATED / NOT CLEAN`
> 기준: `main` / `39122d7f04c239038b4d117a69b97ecf79e06711`
> 보호 범위: 이번 작업에서는 `framework/doc/framework/common/spec/`와 `internals/`를 수정하지 않았다.

## 결론

병합 전 common E2E 375개 ID가 모두 필요한 것으로 확정된 것은 아니다. 문서만으로 실제 process 경계가 없다고 확정할 수 있는 scenario는 없었고, restart, transport, Store, lifecycle, cross-language 또는 최종 client evidence를 요구하는 항목이 대부분이다.

동일한 public 질문 또는 하나의 구성 축으로 분리된 ID 16개를 canonical scenario에 통합했다. 최초 375개에서 5개를 통합한 뒤 남은 후보 17개를 common spec과 다섯 언어 exact interface에 대조하여 11개를 추가 통합하고 6개는 별도 계약으로 유지했다. 현재 한국어·영어 inventory는 각각 359개이며 exact ID·priority parity가 맞는다. 통합한 canonical scenario는 source의 case와 assertion을 variant로 계속 실행한다.

| 분류 | ID 수 | 의미 |
|---|---:|---|
| `PENDING_SCENARIO_REVIEW` | 359 | 현재 독립 scenario다. 필요성과 적합성의 전체 최종 확정 수가 아니다. |
| `CONSOLIDATE_REVIEW` | 0 | 이번 17개 후보의 판정을 완료했다. |
| `MOVE_OWNER_TEST` | 0 | 문서만으로 E2E 제거를 정당화할 후보가 없다. |
| 현재 합계 | 359 | 한국어·영어 canonical inventory다. |
| `MERGED_SOURCE` | 16 | 병합 전 375개에 있던 source ID이며 현재 inventory에서는 제거됐다. |

현재 우선순위 분포는 `P0` 238개, `P1` 110개, `P2` 11개다. 분량의 핵심 원인은 단순 설명 중복만이 아니라 P0 범위 자체가 238개로 넓다는 데 있다.

## 검토 기준과 한계

- 기준 SHA의 병합 전 한국어·영어 config 문서에서 ID, 제목, priority, 검증 질문과 spec link를 추출했다.
- 적용 뒤 한국어와 영어 inventory는 각각 359개이고 ID와 priority parity가 맞는다.
- 작성 지침의 E2E 경계는 둘 이상의 process, transport/discovery/routing, Store 장애, lifecycle, cross-language 또는 multi-hop 중 하나 이상이다. 한 process 반환값만으로 충분하면 owner contract/unit test가 적합하다.
- 기존 scenario의 기동 순서, RID 방향, 분리 배치 변형은 새 ID가 아니라 같은 scenario의 구성 축으로 취급한다.
- 이 문서는 문서 수준 de-duplication audit이다. 17개 후보는 common spec과 다섯 언어 exact interface를 다시 읽어 public terminal, error, lifecycle과 evidence가 같은지 검증했다.
- 작업 중인 spec/internals 변경이 존재하므로 contract 의미는 dirty worktree가 아니라 기준 SHA의 `main`에 고정했다.

근거 지침: `doc/principal/documentation/e2e-scenario-writing-guide.ko.md:167`, `:178`, `:315`, `:474`, `:483`.

## 적용한 확정 통합 16개

Canonical scenario에는 source 쪽의 더 강한 negative evidence, 정상 대조 flow와 cleanup 조건까지 흡수해야 한다. source ID만 지우고 assertion을 버리면 안 된다.

아래 위치는 기준 SHA의 병합 전 위치다.

| source → canonical | source 위치 | canonical 위치 | 중복 판단 |
|---|---|---|---|
| `RL-C2` → `SF-C1` | `framework/doc/framework/common/e2e/config-5-resilience-lifecycle.ko.md:249` | `framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md:141` | 같은 provider crash, owner lease expiry, surviving provider 후속 처리 질문 |
| `RL-F4` → `CH-E2E-05` | `framework/doc/framework/common/e2e/config-5-resilience-lifecycle.ko.md:516` | `framework/doc/framework/common/e2e/config-12-channel-egress-routing.ko.md:251` | 같은 ClientServer server-only outbound NotConfigured 계약 |
| `OBS-C9B` → `RL-F8` | `framework/doc/framework/common/e2e/config-11-observability-ops.ko.md:361` | `framework/doc/framework/common/e2e/config-5-resilience-lifecycle.ko.md:582` | 검증 질문 문장이 동일한 manual-only Relocate preflight 계약 |
| `RL-F2` → `SM-D4A` | `framework/doc/framework/common/e2e/config-5-resilience-lifecycle.ko.md:483` | `framework/doc/framework/common/e2e/config-2-spot-service.ko.md:575` | 같은 rebind 뒤 old Session late relay/disconnect 격리 계약 |
| `TD-A1` → `SA-E2E-20` | `framework/doc/framework/common/e2e/config-8-execution-turn.ko.md:53` | `framework/doc/framework/common/e2e/config-13-submit-admission.ko.md:393` | 같은 one-way submit terminal과 remote handler completion 분리 계약 |
| `RM-C3` → `RM-C7` | `framework/doc/framework/common/e2e/config-1-location-messaging.ko.md:308` | `framework/doc/framework/common/e2e/config-1-location-messaging.ko.md:369` | 동일 select-one의 equal/weighted profile |
| `SM-B5` → `SM-E1` | `framework/doc/framework/common/e2e/config-2-spot-service.ko.md:318` | `framework/doc/framework/common/e2e/config-2-spot-service.ko.md:792` | 동일 missing-handler terminal과 surface별 evidence |
| `SM-D1` → `SM-D2` | `framework/doc/framework/common/e2e/config-2-spot-service.ko.md:515` | `framework/doc/framework/common/e2e/config-2-spot-service.ko.md:530` | 동일 bind·relay·push의 local/remote 배치 variant |
| `RC-A2` → `RC-A1` | `framework/doc/framework/common/e2e/config-4-registration-codec.ko.md:77` | `framework/doc/framework/common/e2e/config-4-registration-codec.ko.md:58` | 언어별 exact scan surface variant |
| `RC-B3` → `RC-B2` | `framework/doc/framework/common/e2e/config-4-registration-codec.ko.md:198` | `framework/doc/framework/common/e2e/config-4-registration-codec.ko.md:182` | 동일 root codec extension의 Protobuf/MessagePack variant |
| `RL-D1` → `PS-B1` | `framework/doc/framework/common/e2e/config-5-resilience-lifecycle.ko.md:281` | `framework/doc/framework/common/e2e/config-3-pubsub.ko.md:125` | 동일 slow-subscriber 격리의 기본/scale profile |
| `SF-G3` → `ST-G2` | `framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md:532` | `framework/doc/framework/common/e2e/config-10-spot-actor-relocation.ko.md:405` | 동일 SpotWide aggregate capacity matrix |
| `TD-B4` → `TD-B1` | `framework/doc/framework/common/e2e/config-8-execution-turn.ko.md:174` | `framework/doc/framework/common/e2e/config-8-execution-turn.ko.md:122` | Yield 중 request/timer callback 진행 variant |
| `TA-A2` → `TA-A1` | `framework/doc/framework/common/e2e/config-9-to-actor-messaging.ko.md:73` | `framework/doc/framework/common/e2e/config-9-to-actor-messaging.ko.md:52` | direct messaging의 bound/unbound state variant |
| `SA-E2E-10` → `SA-E2E-09` | `framework/doc/framework/common/e2e/config-13-submit-admission.ko.md:217` | `framework/doc/framework/common/e2e/config-13-submit-admission.ko.md:199` | 동일 Channel send deadline의 RouteMesh/ClientServer variant |
| `IS-E2E-09` → `IS-E2E-05` | `framework/doc/framework/common/e2e/config-14-instance-spot.ko.md:172` | `framework/doc/framework/common/e2e/config-14-instance-spot.ko.md:106` | Ready-owner crash 뒤 single/concurrent caller variant |

## 후보 17개 독립 검토 결과

Common spec, 다섯 언어 exact interface, public terminal과 역할 server evidence를 대조했다. 같은 public 질문인 11개는 canonical variant matrix로 통합했고, 별도 public surface·실패·continuity를 검증하는 6개는 유지했다.

| source → canonical 후보 | 판정 | 근거 |
|---|---|---|
| `RC-B3` → `RC-B2` | 통합 | codec 종류 variant |
| `SA-E2E-10` → `SA-E2E-09` | 통합 | Channel topology variant |
| `SM-D1` → `SM-D2` | 통합 | owner local/remote 배치 variant |
| `TA-A2` → `TA-A1` | 통합 | Session binding 유무 variant |
| `IS-E2E-09` → `IS-E2E-05` | 통합 | single/concurrent caller variant |
| `ST-I3` → `ST-I2` | 유지 | Actor relocation과 SpotWide aggregate relocation unit·state evidence가 다름 |
| `RM-C3` → `RM-C7` | 통합 | equal/weighted profile variant |
| `RC-A2` → `RC-A1` | 통합 | 언어별 exact scan registration variant |
| `CH-E2E-03` → `CH-E2E-02` | 유지 | nested reply correlation과 Spot/timer serial turn 계약이 다름 |
| `TD-B4` → `TD-B1` | 통합 | Yield 중 진행하는 request/timer callback variant |
| `TD-F2` → `TD-F1` | 유지 | Channel 문맥의 Yield 거부 negative contract |
| `TD-F3` → `TD-F1` | 유지 | Session relay와 Actor FIFO contract |
| `SM-B5` → `SM-E1` | 통합 | missing-handler object surface variant |
| `OBS-C6` → `OBS-C10` | 유지 | RollingUpdate 중 object·binding·state continuity contract |
| `OBS-C7` → `OBS-C10` | 유지 | accepted work·generation continuity contract |
| `RL-D1` → `PS-B1` | 통합 | subscriber 수와 load profile variant |
| `SF-G3` → `ST-G2` | 통합 | aggregate capacity bucket/profile variant |

## 유사하지만 합치지 않는 대표 쌍

문자열 유사도나 같은 spec link만으로 합치면 계약을 잃는 사례다.

- `TD-A2`와 `TD-B1`: 같은 Spot의 대기 상황이지만 Async는 다음 callback을 막고 Yield는 진행시킨다. 기대 결과가 반대다.
- `SM-C1`과 `SM-C2`: Channel → Spot과 Spot → Channel로 call direction과 원래 reply owner가 다르다.
- `PS-B2`와 `PS-D4`: same publisher restart와 replacement identity 전환은 stale identity와 replay 경계가 다르다.
- `SM-A9`와 `SM-B11`: User Spot initialize publication과 Actor initial membership publication은 준비 barrier와 lifecycle 근거가 다르다.
- `TD-C3`와 `TD-C5`: I/O wait가 CPU slot을 소비하지 않는 것과 CPU saturation 중 I/O progress는 서로 보완하지만 장애 조건과 성공 질문이 다르다.
- `PS-E2A`와 `PS-E2B`: Store prerequisite 누락과 automatic/manual mode 혼합은 서로 다른 configuration error다.

## 현재 inventory gate에서 확인한 별도 문제

`framework/languages/cpp/e2e/verify_common_inventory.sh`의 고정 scenario 수 374는 제거했다. gate는 이제 한국어·영어 ID parity를 먼저 비교하고 실제 영어 canonical ID 집합을 언어 inventory와 대조한다.

최근 실행 결과는 다음과 같다. 이것은 scenario 구현 완료 증거가 아니라 inventory/source 연결 상태를 보여 주는 정적 gate다.

```text
configs=14 scenarios=359
feature-map-missing=97
source-missing=125
incomplete-status=57
FAIL: 279 required inventory conditions are open
```

이는 common 한국어·영어 inventory parity와 동적 scenario 수 계산은 맞지만 실제 C++ feature map/source gap
279건이 남았다는 뜻이다. 언어별 runner와 source ID 통합은 이번 문서 통합 범위에 포함하지 않았다.

## 병합 전 375개 전수 분류

### config-1 (17개)

- `KEEP_E2E` (16): `RM-A1` `RM-A2` `RM-A3` `RM-A4` `RM-A6` `RM-A7` `RM-B1` `RM-B2` `RM-B3` `RM-C1` `RM-C2` `RM-C4` `RM-C5` `RM-C7` `RM-C8` `RM-C9`
- `MERGE_CONFIRMED` (1): `RM-C3`→`RM-C7`
- `CONSOLIDATE_REVIEW` (0): 없음

### config-2 (66개)

- `KEEP_E2E` (64): `SM-A1` `SM-A2` `SM-A3` `SM-A4` `SM-A5` `SM-A6` `SM-A7` `SM-A8` `SM-A9` `SM-A10` `SM-A11` `SM-A12` `SM-A13` `SM-B0` `SM-B0A` `SM-B1` `SM-B2` `SM-B3` `SM-B4` `SM-B6` `SM-B7` `SM-B8` `SM-B9` `SM-B10` `SM-B11` `SM-C1` `SM-C2` `SM-C3` `SM-C4` `SM-C5` `SM-C6` `SM-D2` `SM-D3` `SM-D4` `SM-D4A` `SM-D4B` `SM-D5` `SM-D5A` `SM-D6` `SM-D7` `SM-D8` `SM-D9` `SM-D10` `SM-D11` `SM-D12` `SM-D13` `SM-D14` `SM-D15` `SM-E1` `SM-E2` `SM-E3` `SM-E4` `SM-F1` `SM-F2` `SM-F3` `SM-F4` `SM-F5` `SM-F6` `SM-G1` `SM-G2` `SM-G3` `SM-G4` `SM-G5A` `SM-G5B`
- `MERGE_CONFIRMED` (2): `SM-B5`→`SM-E1` `SM-D1`→`SM-D2`
- `CONSOLIDATE_REVIEW` (0): 없음

### config-3 (24개)

- `KEEP_E2E` (24): `PS-A1` `PS-A2` `PS-A3` `PS-A4` `PS-B1` `PS-B2` `PS-D1` `PS-D2` `PS-D3` `PS-D4` `PS-D5` `PS-D6` `PS-D7A` `PS-D7B` `PS-E1` `PS-E2A` `PS-E2B` `PS-E2C` `PS-F1` `PS-F2` `PS-F3` `PS-F4` `PS-F5` `PS-C1`
- `MERGE_CONFIRMED` (0): 없음
- `CONSOLIDATE_REVIEW` (0): 없음

### config-4 (12개)

- `KEEP_E2E` (10): `RC-A1` `RC-A3` `RC-A4` `RC-A5` `RC-A6` `RC-B1` `RC-B2` `RC-B4` `RC-B5` `RC-B6`
- `MERGE_CONFIRMED` (2): `RC-A2`→`RC-A1` `RC-B3`→`RC-B2`
- `CONSOLIDATE_REVIEW` (0): 없음

### config-5 (39개)

- `KEEP_E2E` (35): `RL-A1` `RL-A2` `RL-A3` `RL-A4` `RL-A5` `RL-B1` `RL-B2` `RL-B3` `RL-B4` `RL-B5` `RL-B6` `RL-C1` `RL-C3` `RL-C4` `RL-D2` `RL-D3` `RL-D4` `RL-D5` `RL-E1` `RL-E2` `RL-E3` `RL-E4` `RL-E5` `RL-F1` `RL-F3` `RL-F5` `RL-F6` `RL-F7` `RL-F8` `RL-F9` `RL-F10` `RL-F11` `RL-F12` `RL-F13` `RL-F14`
- `MERGE_CONFIRMED` (4): `RL-C2`→`SF-C1` `RL-F2`→`SM-D4A` `RL-F4`→`CH-E2E-05` `RL-D1`→`PS-B1`
- `CONSOLIDATE_REVIEW` (0): 없음

### config-6 (29개)

- `KEEP_E2E` (28): `SF-A1` `SF-A2` `SF-B1` `SF-B2` `SF-B3` `SF-C1` `SF-C2` `SF-C3` `SF-C4` `SF-C5` `SF-C5A` `SF-D1` `SF-D2` `SF-D3` `SF-E1` `SF-F1` `SF-F2` `SF-F3` `SF-F4` `SF-F5` `SF-F6` `SF-F7` `SF-F8` `SF-F9` `SF-F10` `SF-F11` `SF-G1` `SF-G2`
- `MERGE_CONFIRMED` (1): `SF-G3`→`ST-G2`
- `CONSOLIDATE_REVIEW` (0): 없음

### config-7 (12개)

- `KEEP_E2E` (12): `MON-A1` `MON-A2` `MON-A3` `MON-A4A` `MON-A4B` `MON-A5` `MON-A6` `MON-B1` `MON-B2` `MON-C1` `MON-D1A` `MON-D1B`
- `MERGE_CONFIRMED` (0): 없음
- `CONSOLIDATE_REVIEW` (0): 없음

### config-8 (32개)

- `KEEP_E2E` (30): `TD-A2` `TD-A3` `TD-A4` `TD-A5` `TD-B1` `TD-B2` `TD-B3` `TD-C1` `TD-C2` `TD-C3` `TD-C4` `TD-C5` `TD-D1` `TD-D2` `TD-D3` `TD-D4` `TD-D5` `TD-D6` `TD-E1` `TD-E2` `TD-E2A` `TD-E3` `TD-F1` `TD-F2` `TD-F3` `TD-F4` `TD-F5` `TD-F5A` `TD-F6` `TD-G1`
- `MERGE_CONFIRMED` (2): `TD-A1`→`SA-E2E-20` `TD-B4`→`TD-B1`
- `CONSOLIDATE_REVIEW` (0): 없음

### config-9 (7개)

- `KEEP_E2E` (6): `TA-A1` `TA-A3` `TA-A4` `TA-B1` `TA-B2` `TA-B3`
- `MERGE_CONFIRMED` (1): `TA-A2`→`TA-A1`
- `CONSOLIDATE_REVIEW` (0): 없음

### config-10 (43개)

- `KEEP_E2E` (43): `ST-A1` `ST-A2` `ST-A3` `ST-B1` `ST-B2` `ST-B3` `ST-B4` `ST-C1` `ST-C2` `ST-C3` `ST-D1` `ST-D2` `ST-E1` `ST-E1B` `ST-E1C` `ST-E1A` `ST-E2` `ST-F1` `ST-F2` `ST-F3` `ST-F3A` `ST-F4` `ST-F5` `ST-F6` `ST-G1` `ST-G2` `ST-G3` `ST-G4` `ST-G5` `ST-G6` `ST-H1` `ST-H2` `ST-H3` `ST-H4` `ST-H4A` `ST-H4B` `ST-H5` `ST-I1` `ST-I2` `ST-I3` `ST-I4` `ST-I5` `ST-I6`
- `MERGE_CONFIRMED` (0): 없음
- `CONSOLIDATE_REVIEW` (0): 없음

### config-11 (22개)

- `KEEP_E2E` (21): `OBS-A1` `OBS-A2` `OBS-A3` `OBS-A4` `OBS-A5` `OBS-B1` `OBS-B2` `OBS-B3` `OBS-B4` `OBS-C1` `OBS-C2` `OBS-C3` `OBS-C4` `OBS-C5` `OBS-C6` `OBS-C7` `OBS-C8` `OBS-C9A` `OBS-C10` `OBS-C11` `OBS-C12`
- `MERGE_CONFIRMED` (1): `OBS-C9B`→`RL-F8`
- `CONSOLIDATE_REVIEW` (0): 없음

### config-12 (16개)

- `KEEP_E2E` (16): `CH-E2E-01` `CH-E2E-02` `CH-E2E-03` `CH-E2E-06` `CH-E2E-07A` `CH-E2E-07B` `CH-E2E-07C` `CH-E2E-11` `CH-E2E-04A` `CH-E2E-04B` `CH-E2E-04C` `CH-E2E-05` `CH-E2E-10` `CH-E2E-12` `CH-E2E-08` `CH-E2E-09`
- `MERGE_CONFIRMED` (0): 없음
- `CONSOLIDATE_REVIEW` (0): 없음

### config-13 (20개)

- `KEEP_E2E` (19): `SA-E2E-01` `SA-E2E-02` `SA-E2E-03` `SA-E2E-04` `SA-E2E-05` `SA-E2E-06` `SA-E2E-07` `SA-E2E-08` `SA-E2E-09` `SA-E2E-11` `SA-E2E-12` `SA-E2E-13` `SA-E2E-14` `SA-E2E-15` `SA-E2E-16` `SA-E2E-17` `SA-E2E-18` `SA-E2E-19` `SA-E2E-20`
- `MERGE_CONFIRMED` (1): `SA-E2E-10`→`SA-E2E-09`
- `CONSOLIDATE_REVIEW` (0): 없음

### config-14 (36개)

- `KEEP_E2E` (35): `IS-E2E-01` `IS-E2E-02` `IS-E2E-03` `IS-E2E-04` `IS-E2E-05` `IS-E2E-06` `IS-E2E-07` `IS-E2E-08` `IS-E2E-10` `IS-E2E-11` `IS-E2E-12` `IS-E2E-13` `IS-E2E-14` `IS-E2E-15` `IS-E2E-16` `IS-E2E-17` `IS-E2E-18` `IS-E2E-19` `IS-E2E-20` `IS-E2E-21` `IS-E2E-22` `IS-E2E-23` `IS-E2E-24` `IS-E2E-25` `IS-E2E-26` `IS-E2E-27` `IS-E2E-28` `IS-E2E-29` `IS-E2E-30` `IS-E2E-31` `IS-E2E-32` `IS-E2E-33` `IS-E2E-34` `IS-E2E-35` `IS-E2E-36`
- `MERGE_CONFIRMED` (1): `IS-E2E-09`→`IS-E2E-05`
- `CONSOLIDATE_REVIEW` (0): 없음

## 후속 작업 순서

1. 다섯 언어 feature map과 runner에서 11개 previous selector를 canonical ID와 variant로 이관한다.
2. Source test와 package 증거를 갱신한 뒤 canonical scenario별 실제 process evidence를 실행한다.
3. 언어별 aggregate와 cross-language 결과가 모두 닫힌 뒤에만 `CLEAN`을 판단한다.

## 판정

현재 판정은 `NOT CLEAN`이다. 17개 추가 후보의 contract 및 exact-interface 판정과 11개 common 문서
통합은 끝났지만, 다섯 언어 selector·source 이관과 process 검증은 수행하지 않았다. 정적 C++ inventory
gate에도 구현 gap 279건이 남아 있다. 이번 완료 범위는 누적 16개 source ID의 canonical common 문서
통합이며, process E2E 재검증이나 현재 359개 전체의 적합성 승인을 뜻하지 않는다.
