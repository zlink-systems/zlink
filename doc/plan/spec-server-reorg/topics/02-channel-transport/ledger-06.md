# 대장 — `06-wire-protocol.ko.md` (옛 51)

> 원본: `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md`
> 새 문서: `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md`
>
> 재작성은 절 번호·제목을 원문 그대로 유지했다(다른 문서 12개가 `#절-번호` anchor로 이
> 문서를 인용하므로 — mapping §6). 문장 층위 정리만 했다 — `**결정**` 라벨 제거(원문에 없었음),
> §12 검증 절을 인터페이스 관찰 형식(가이드 §9.3)으로 재구성, "정본" → "유일한 기준"으로
> 어휘 교체, byte 배치(계약 서술)와 그것을 만드는 절차(구현 서술)의 구분을 §0 머리말과
> §2 Frame 구성에 명시.

## R# → 새 위치

R150~R202는 mapping §5.6이 이미 옛 절 번호로 새 위치를 지정했다(절 번호가 안 바뀌므로 옛 위치 =
새 위치). 아래는 그 표에 실제 배치를 확인(grep)한 결과다.

| R# | 새 위치 | 스펙 소유 | 비고 |
|---|---|---|---|
| R150 | §1 규범 생성 기준 | 이 문서 | "정본" → "유일한 기준"으로 어휘만 교체 |
| R151 | §1 계층별 규범 형식 | 이 문서(schema) + 3개 소유 문서 링크 | 표 그대로 유지 |
| R152 | §1 Validator | 이 문서 | validator 명령·wire major·capability·실패 조건 그대로 |
| R153 | §1 Location Store authority key 형식 | schema + Location runtime | key 형식·percent encoding 규칙 그대로 |
| R154 | §2 Frame 구성 | 이 문서(schema가 byte 배치 소유) | frame 순서 문장 앞에 계약/구현 구분 한 문장 추가(원문 내용 변경 없음) |
| R155 | §2 Decode 검증과 크기 상한 | 이 문서 | 4,294,966,774 byte 등 수치 그대로 |
| R156 | §2 Decode 검증과 크기 상한(ClientServer complete-message 상한 하위 문단) | 이 문서 | 예시 값(32 MiB/17 MiB) 그대로 |
| R157 | §2 Typed payload envelope | 이 문서 | |
| R158 | §2 Framework multipart application profile | 이 문서 | encoding 순서 3단계 그대로 |
| R159 | §2 Framework multipart application profile(마지막 문단) | 이 문서 | byte HWM 재계산 안 함, job queue permit 문장 그대로 |
| R160 | §3 Command space 머리말 | 이 문서(schema) | reserved ID 목록 그대로 |
| R161 | §3.1 Body 구성 + Route 검증 | 이 문서 | hop count 1..8, 16 MiB 상한 등 그대로 |
| R162 | §3.1 통지 중복 억제 | 이 문서(45와 상세 링크 공유) | 상태 전이 idle→inFlight→sentUntilExpiry 그대로 |
| R163 | §3.2 Bound session 교체 notification | 이 문서 | 100 ms 지연 규칙 그대로 |
| R164 | §4 Admission 절차 | 이 문서 | |
| R165 | §4 DescriptorRevision ordering | 이 문서 | |
| R166 | §4 Physical connection replacement | 이 문서 | |
| R167 | §4 ClientServer 방향 | 이 문서 | |
| R168 | §5 Probe와 Ack 주기 | 이 문서 | 양방향 probe 의무·epoch 규칙 문단 그대로 |
| R169 | §5 Classic fanout beacon + Subscriber ready 판정 | 이 문서 | topic/payload byte 값 그대로 |
| R170 | §6 `framework-json-v1` profile 규칙 + Relocation adapter state는 profile 밖 | 이 문서(계약은 04-message-model §2.3 소유) | |
| R171 | §7 Generation과 Authority | 이 문서 | |
| R172 | §7 Creation record | 이 문서 | |
| R173 | §7 Factory 실패 처리 + Object role | 이 문서 | |
| R174 | §8 머리말(recovery 적용 범위) | 이 문서(범위는 31 §4.4 소유) | |
| R175 | §8 Missing+Instance intent envelope + Command 39 route kind | 이 문서 | |
| R176 | §8 Target host의 scan과 recovery | 이 문서 | |
| R177 | §8 Cold activation recovery 실패 처리 | 이 문서 | |
| R178 | §8.1 Command 47 — remote create / Command 48 — remote close | 이 문서 | |
| R179 | §8.1 Reply envelope | 이 문서 | |
| R180 | §9 Actor join 요청 envelope(1문단) | 이 문서 | |
| R181 | §9 Actor join 요청 envelope(2문단, multipart 감싸기) | 이 문서 | |
| R182 | §9 수신자 stable-type 해석 | 이 문서 | 5조건 목록 그대로 |
| R183 | §9 Session seal과 source relay | 이 문서 | |
| R184 | §9 Relocation manifest와 direct chunk transfer(1~2번째 불릿) | 이 문서 | |
| R185 | §9 Relocation manifest와 direct chunk transfer(3~4번째 불릿) | 이 문서 | |
| R186 | §9 Relocation manifest와 direct chunk transfer(5~6번째 불릿) | 이 문서 | RelocationCutoverWaitTimeout 기본값 1,000 ms 그대로 |
| R187 | §9 Relocation manifest와 direct chunk transfer(7~9번째 불릿) | 이 문서 | |
| R188 | §9 CRC-32C 규약과 capability | 이 문서 | polynomial·initial·XOR 값, golden fixture 경로 그대로 |
| R189 | §9 Target CAS와 남은 Store 역할(1~3번째 불릿) | 이 문서 | |
| R190 | §9 Target CAS와 남은 Store 역할(4~5번째 불릿) | 이 문서 | |
| R191 | §10 `RelocationId` + Authority와 target-only CAS | 이 문서 | |
| R192 | §10 Commit 뒤 queue와 Ready(5단계 목록 + 전역 순서 문단) | 이 문서 | |
| R193 | §10 Commit 뒤 queue와 Ready(source 관점 문단) | 이 문서 | |
| R194 | §10 Session route(1~4번째 불릿) | 이 문서 | |
| R195 | §10 Session route(5~7번째 불릿) | 이 문서 | SessionRelocationSealTimeout 기본값 3,000 ms 그대로 |
| R196 | §10 마지막 문단(각각 한 번만 수행) | 이 문서 | |
| R197 | §11 `OperationId`와 `ReplyRouteId` | 이 문서 | |
| R198 | §11 Terminal completion 추적 | 이 문서 | |
| R199 | §11 `Completed` 조건 | 이 문서 | |
| R200 | §11 Root replacement | 이 문서 | |
| R201 | §11 `SendReady` record와 binding completion | 이 문서(Core 0.13 send_completion은 Core 소유) | |
| R202 | 말미 "Wire record와 shared capacity" | 46·50이 각각 소유, 이 문서는 링크만 | |

§12 구현 검증 12항목은 개별 R로 대장에 올리지 않고 인터페이스 관찰 checklist(가이드 §9.3)로
그대로 옮겼다(mapping §5.6 각주대로) — 4개 소제목(Schema와 codec / Decode와 admission /
Relocation 전송과 CAS / Terminal completion)으로 묶었으며 문장 자체는 원문 12개 항목과
1:1 대응이다.

## 이동 후 갱신할 링크

새 문서 안에서 옛 flat 경로를 그대로 쓰는 링크. 이 7개 문서(02-channel-transport)를 제외한
나머지 spec/server 문서가 아직 주제 디렉터리로 옮겨지지 않았으므로 `../NN-slug.ko.md`로
경로만 보정했다. 캠페인 §5(마지막 이동 단계)에서 그 문서들이 옮겨지면 아래 링크도 다시
갱신해야 한다.

| 문서 | 새 문서에서의 링크 | 갱신 필요 시점 |
|---|---|---|
| 계층 경계와 식별자 | `../40-internal-layering.ko.md` | 01-execution 주제 이동 시 |
| Location runtime | `../21-location-runtime.ko.md` | 05-location-relocation 주제 이동 시 |
| Redis Relocation Store | `../23-relocation-store-redis.ko.md` | 05-location-relocation 주제 이동 시 |
| Relocation handoff 상태 전이 | `../52-internal-relocation-handoff.ko.md` | 05-location-relocation 주제 이동 시 |
| 45. target 선택과 route cache | `../45-internal-routing-and-cache.ko.md#2-이동과-캐시가-만나는-지점--성능-절벽` | 05-location-relocation 주제 이동 시 |
| 13. Mesh Node | `../13-mesh-node.ko.md` | 03-spot-actor 주제 이동 시 |
| Message model §2.3 | `../04-message-model.ko.md#23-framework-json-v1-typed-payload-profile` | 01-execution 주제 이동 시 |
| 장애 대응과 failover 범위 §4.4 | `../31-failure-failover-policy.ko.md#44-instance-spot-cold-activation과-owner-장애를-구분한다` | 03-spot-actor 주제 이동 시 |
| 15. Spot과 Actor 모델 §4.2 | `../15-spot-actor.ko.md#42-다른-node의-spot으로-actor를-join하는-순서` | 03-spot-actor 주제 이동 시 |
| 수신과 dispatch loop | `../46-internal-dispatch-loop.ko.md` | 01-execution 주제 이동 시 |
| Payload 소유권과 복사 | `../50-internal-message-ownership.ko.md` | 01-execution 주제 이동 시 |

같은 주제 안 sibling 링크는 이미 새 경로다 — `Transport liveness`는 `05-transport-liveness.ko.md`,
`ClientServer Channel`은 `03-client-server-channel.ko.md`.

옛 문서를 `#절-번호` anchor로 인용하는 외부 문서 12개(mapping §6, 코디네이터가 처리)는 이
대장의 범위 밖이다 — 절 번호·제목을 유지했으므로 anchor 자체는 안 바뀌고 경로만 바뀐다.

## spec-gap 후보

새로 발견한 것 없음. mapping §4 S8·S9·S10(구조 배치 재검토 대상)과 spec-gap 후보 G-candidate 1
(Classic fanout 수신 상한 3축 값 미정, 29 §4/51 §5 동일)은 mapping 문서에 이미 기록되어
있으며 이 문서 재작성 중 추가로 나온 gap은 없다.
