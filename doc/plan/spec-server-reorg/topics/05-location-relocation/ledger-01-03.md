# 규칙 등가성 대장 — 01-location-runtime · 02-location-store-redis · 03-relocation-store-redis

`framework/doc/framework/common/spec/server/05-location-relocation/{01-location-runtime,02-location-store-redis,03-relocation-store-redis}.ko.md`
재작성 결과. R1–R176만 다룬다(R177 이후는 `04-relocation-flow`·`05-host-relocation-flow`·
`06-failure-failover-policy` 담당 에이전트가 채운다). 각 행은 새 문서에서 grep으로 핵심 문구를
실물 확인한 뒤에만 채웠다(가이드 §2.5). 미확인 행은 없다 — §9.2(구 21 §7.4, Target이 새
message를 받기 시작하는 시점)가 최초 재작성에서 누락된 것을 발견해 `01-location-runtime.ko.md`에
새 §9.2로 추가하고 이후 절 번호를 재조정했다(§9.3·§9.4로 한 칸씩 밀림).

## 01-location-runtime.ko.md (R1–R113, 구 21)

| R# | 새 위치 | 비고 |
|---|---|---|
| R1 | §1 | |
| R2 | §1.1 | |
| R3 | §1.1 | |
| R4 | §1.1 | |
| R5 | §1.2 | |
| R6 | §1.2 | |
| R7 | §1.2 | |
| R8 | §1.2 | |
| R9 | §1.2 | |
| R10 | §1.2 | 링크를 `02-location-store-redis.ko.md#8-…`·`03-relocation-store-redis.ko.md#8-…`로 갱신 |
| R11 | §1.2 | |
| R12 | §2 | |
| R13 | §2 | |
| R14 | §2 | |
| R15 | §2 | |
| R16 | §1.3 | 8단계 개요만 유지, 상세 mechanics는 `04-relocation-flow.ko.md` 링크로 대체(§4 S7) |
| R17 | §1.3 | |
| R18 | §1.3 | |
| R19 | §3.1 | |
| R20 | §3.1 | |
| R21 | §3.2 | |
| R22 | §3.2 | |
| R23 | §3.3 | |
| R24 | §3.3 | |
| R25 | §3.3 | |
| R26 | §3.3 | |
| R27 | §3.4 | 골든 fixture 링크를 6단계 상대경로로 갱신(주제 디렉터리 한 단계 추가) |
| R28 | §3.4 | |
| R29 | §3.4 | |
| R30 | §3.4 | |
| R31 | §3.4 | |
| R32 | §3.4 | |
| R33 | §3.4 (MeshNode descriptor 소제목) | S4: 소제목으로 분리 |
| R34 | §3.4 (ClientServer server descriptor 소제목) | S4 |
| R35 | §3.4 (Fanout publisher descriptor 소제목) | S4 |
| R36 | §3.4 (Authority record 소제목) | S4 |
| R37 | §3.4 (Authority record 소제목) | S4 |
| R38 | §3.4 | |
| R39 | §4 | |
| R40 | §4 | |
| R41 | §4 | |
| R42 | §4 | |
| R43 | §4 | |
| R44 | §4 | |
| R45 | §4 | |
| R46 | §4 | |
| R47 | §4 | |
| R48 | §4 | |
| R49 | §5 | |
| R50 | §5 | |
| R51 | §5 | |
| R52 | §5 | |
| R53 | §6 | |
| R54 | §6.1 | |
| R55 | §6.1 | |
| R56 | §6.1 | |
| R57 | §6.2 | |
| R58 | §6.2 | |
| R59 | §7 | |
| R60 | §7 | |
| R61 | §7 | |
| R62 | §7 | |
| R63 | §7 | |
| R64 | §7 | |
| R65 | §7 | |
| R66 | §7 | |
| R67 | §7.1 | |
| R68 | §7.1 | |
| R69 | §7.1 | |
| R70 | §7.1 | |
| R71 | §7.1 | |
| R72 | §7.1 | |
| R73 | §7.1 | |
| R74 | §7.2 | |
| R75 | §7.2 | |
| R76 | §7.2 | |
| R77 | §7.3 | |
| R78 | §7.3 | |
| R79 | §7.3 | |
| R80 | §7.4 | 옛 24 §2.2 링크를 `../06-observability/01-runtime-monitoring.ko.md#5-topology-상태--routemeshclientserverautomatic-fanout`로 갱신 |
| R81 | §7.4 | 위와 같은 링크 갱신 |
| R82 | §8 (도입 문단) | 상세 handoff는 `04-relocation-flow.ko.md` 링크(§4 S7) |
| R83 | §8.1 | `51-internal-service-wire-protocol.ko.md` 링크는 옛 경로 그대로(§6 이관 대상) |
| R84 | §8.1 | |
| R85 | §8.1 | |
| R86 | §8.1 | |
| R87 | §8 (도입 문단 + 새 다이어그램) | S7: 옛 §1.4·§7.3의 중복 mermaid 2개를 제거하고, owner lease 갱신 → target CAS → owner lease 갱신 흐름을 보여주는 다이어그램 1개로 대체(가이드 §7.2) |
| R88 | §8.2 | |
| R89 | §8.2 | |
| R90 | §8.2 | |
| R91 | §9.1 | |
| R92 | §9.1 | |
| R93 | §9.1 | |
| R94 | §9.1 | |
| R95 | §9.1 | |
| R96 | §9.1 | |
| R97 | §9.1 | |
| R98 | §9.2 | 최초 재작성에서 누락됐던 구 §7.4 내용 — 리뷰 중 발견해 새 §9.2로 추가(위 안내 참고) |
| R99 | §9.2 | 위와 같음 |
| R100 | §9.2 | 위와 같음 |
| R101 | §9.3 | |
| R102 | §9.3 | |
| R103 | §9.3 | |
| R104 | §9.4 | Session owner가 하는 부분(exact matching seal 해제 → held message를 source route로 제출)만
[session/02 §8](../../../../../framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md#8-actor-relocation-중-session의-책임)
링크로 대체. Source 쪽 6단계(temp queue 폐기, ingress hold 원본 복원, command 44 abort one-way,
Store 읽기·쓰기 없음, source 재개)는 본문에 그대로 유지(§4 S1) |
| R105 | §10 | |
| R106 | §10 | |
| R107 | §10 | |
| R108 | §10 | |
| R109 | §10 | |
| R110 | §10 | |
| R111 | §10 | |
| R112 | §11 | |
| R113 | §11 | |

## 02-location-store-redis.ko.md (R114–R144, 구 22)

| R# | 새 위치 | 비고 |
|---|---|---|
| R114 | §1 | |
| R115 | §1 | |
| R116 | §2 | |
| R117 | §2 | SPI package 원칙 문장은 `01-location-runtime.ko.md#21-…`로 링크·통합(§4 S8), 이 문서는 3-operation 목록만 서술 |
| R118 | §2 | |
| R119 | §2 | |
| R120 | §3 | |
| R121 | §3 | |
| R122 | §3 | |
| R123 | §4 | |
| R124 | §4 | |
| R125 | §4 | |
| R126 | §4 | spec-gap 후보 — 아래 참고 |
| R127 | §5 | |
| R128 | §5 | |
| R129 | §5 | |
| R130 | §6 | |
| R131 | §6 | |
| R132 | §6 | |
| R133 | §7 | |
| R134 | §8 | 원문은 §7 서두에 있었으나 Redis 구현 문장이므로 §8(공식 Redis provider 절)로 이동 |
| R135 | §8 | |
| R136 | §8 | |
| R137 | §8 | |
| R138 | §8 | |
| R139 | §9 | |
| R140 | §9 | |
| R141 | §9 | |
| R142 | §9 | |
| R143 | §9 | |
| R144 | §9 | |

## 03-relocation-store-redis.ko.md (R145–R176, 구 23)

| R# | 새 위치 | 비고 |
|---|---|---|
| R145 | §1 | |
| R146 | §1 | |
| R147 | §1 | |
| R148 | §2 | SPI package 원칙 문장은 `01-location-runtime.ko.md#21-…`로 링크(§4 S8) |
| R149 | §2 | |
| R150 | §2 | |
| R151 | §3 | |
| R152 | §3 | |
| R153 | §3 | |
| R154 | §3 | |
| R155 | §3 | |
| R156 | §3 | |
| R157 | §3 | |
| R158 | §4.1 | |
| R159 | §4.2 | |
| R160 | §4.3 | |
| R161 | §4.4 | |
| R162 | §5 | |
| R163 | §5 | |
| R164 | §5 | |
| R165 | §5 | |
| R166 | §5 | |
| R167 | §6 | |
| R168 | §7 | |
| R169 | §8 | |
| R170 | §8 | |
| R171 | §8 | |
| R172 | §8 | |
| R173 | §8 | |
| R174 | §8 | "묶지 않는다" 원칙은 `01-location-runtime.ko.md#2-…`가 소유, §7에서도 짧게 인용(§4 S9) |
| R175 | §8 | |
| R176 | §8 | |

## 나중에 anchor를 붙일 링크

병렬로 작성 중인 `04-relocation-flow.ko.md`·`05-host-relocation-flow.ko.md`에는 지침에 따라
파일명만 걸고 `#anchor`를 붙이지 않았다. 각 문서가 확정되면 다음 링크에 anchor를 채운다.

| 이 문서의 위치 | 대상 | 현재 링크 |
|---|---|---|
| 01 §1.3 마지막 문단 | 04의 handoff 상세 절 | `04-relocation-flow.ko.md` |
| 01 §8 도입 | 04의 handoff 상세 절 | `04-relocation-flow.ko.md` |
| 01 §8.1 | `51-internal-service-wire-protocol.ko.md#9-…`(옛 경로, 아직 이동 안 됨) | 아래 "이동 후 갱신할 링크" 참고 |
| 01 §8.2 마지막 문단 | 04 §4 "정상 처리 순서" | `04-relocation-flow.ko.md` |
| 01 §9 도입 | 05 §7 "Relocation unit과 실행 순서" | `05-host-relocation-flow.ko.md#7-…`(파일명은 확정, 헤더 문구·anchor는 05 완료 후 확인) |
| 01 §9 도입 | 04(payload chunk 분할·전송 예산·실패 규칙) | `04-relocation-flow.ko.md` |
| 01 §11 도입 | 05 (Host 명령 상태·최종 결과) | `05-host-relocation-flow.ko.md` |
| 01 §12 마지막 문단 | 05 「17. 구현 및 contract test 검증 요구」 | anchor는 이미 채웠으나(05가 이미 존재) 05가 다시 수정되면 재확인 필요 |
| 03 §1 | 04(handoff payload 전달·검증 규칙) | `04-relocation-flow.ko.md` |

## 이동 후 갱신할 링크

캠페인 §5(en 작성·파일 이동)가 실행될 때 다음 링크를 새 경로로 함께 치환한다. 지금은 옛
문서가 아직 그 경로에 있으므로 옛 경로 그대로 둔다(작업 지침의 "다른-주제 문서는
`../<NN-slug>.ko.md`" 규칙에 따름).

| 위치 | 현재(옛) 링크 | 이동 후 갱신 대상 |
|---|---|---|
| 01 §8.1 | `../51-internal-service-wire-protocol.ko.md#9-maintenance-capture와-relocation-envelope` | `51-internal-service-wire-protocol`이 속할 새 주제 경로로 |
| `testdata/location/redis/actor-location-v2.json` 등 5개(mapping §6 확인) | `"notice"` 필드의 `22-location-store-redis.ko.md` 경로 | `05-location-relocation/02-location-store-redis.ko.md` |
| `authority-store-v3.json` | notice 필드에 경로 문자열 없음(§1 재확인 필요) | 내용 확인 후 필요 시 같은 갱신 |
| 스펙 안팎에서 21·22·23을 참조하는 문서 236개(mapping §1 집계) | `NN-location-…ko.md` 형태의 옛 경로 | `05-location-relocation/0{1,2,3}-….ko.md` + 절 anchor 치환표(mapping §6) |
| mkdocs nav / redirect | 없음(아직 미등록) | mapping §6의 redirect 표대로 등록 |

## spec-gap 후보

새로 발견한 실제 스펙 결함은 없다. mapping.ko.md §"spec-gap 후보"가 이미 결론 낸 대로, 이 세
문서 범위에서 조사 중 나온 항목은 문서 구조 문제(§4 S1~S17)이며 재작성으로 해소됐다. 다음
두 건은 판정이 필요 없는 관찰 사항으로만 남긴다(원문 자체의 표현이며 재작성이 만든 것이
아니다).

1. **21 §6.4(→ 01 §7.4)의 "[50 Runtime monitoring]" 표기.** 옛 문서 원문이 이미 전역 번호
   "50"을 텍스트에 남긴 상태였다(링크 대상 파일명은 `24-runtime-monitoring.ko.md`로 정확했다).
   재작성본에서는 이 낡은 전역 번호를 텍스트에서 빼고 "Runtime 상태와 운영 진단 §5"로만
   표기해 새 경로(`../06-observability/01-runtime-monitoring.ko.md#5-…`)와 어긋나지 않게
   했다. 계약 내용 변경은 아니다.
2. **21 §1.2(→ 01 §1.2)의 "User Spot 하나에 속한 Actor 총수 | `1,024`로 제한하지 않는다"**는
   무엇을 부정하는지 원문 자체가 명시하지 않는다(어디서도 1,024라는 상한을 먼저 주장한 적이
   없다). 22 R126(→ 02 §4)은 같은 사실을 "batch의 2,048-key 제한으로 정하지 않는다"로 다르게
   표현한다 — 두 문장이 서로 다른 잠재 상한(1,024 vs 2,048-key)을 부정 대상으로 암시하는 것처럼
   읽힐 수 있으나, 원문 표현을 그대로 옮겼을 뿐 재작성이 만든 모순은 아니다. 판정 없이 관찰만
   남긴다.
