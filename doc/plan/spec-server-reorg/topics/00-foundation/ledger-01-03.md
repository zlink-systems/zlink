# 00-foundation — 01·03 문서 대장

대상: `01-public-contract-governance.ko.md`(R1~R22), `03-overview.ko.md`(R23~R39).
매핑표: [mapping.ko.md](mapping.ko.md) §5.

각 행의 "새 위치"는 새 문서에서 핵심 문구를 grep으로 실물 확인한 뒤에만 채웠다.

| R# | 새 위치 (절 번호·제목) | 비고 |
|---|---|---|
| R1 | 01 §1. 공개 계약이란 무엇인가 | |
| R2 | 01 §2. 계약 소유권 (4행 표) | |
| R3 | 01 §2. 계약 소유권 | |
| R4 | 01 §2. 계약 소유권 (굵은 규칙 불릿) | |
| R5 | 01 §2. 계약 소유권 (굵은 규칙 불릿) | |
| R6 | 01 §3. Production source owner | |
| R7 | 01 §3. Production source owner | |
| R8 | 01 §3. Production source owner | |
| R9 | 01 §3. Production source owner | |
| R10 | 01 §3. Production source owner | |
| R11 | 01 §3. Production source owner | |
| R12 | 01 §3. Production source owner | |
| R13 | 01 §3. Production source owner (후단 산문) | |
| R14 | 01 §4. 새 계약에 고정할 항목 (8항목 불릿) | |
| R15 | 01 §5. 공개 계약 절차 (7단계) | |
| R16 | 01 §5. 공개 계약 절차 | |
| R17 | 01 §6. 언어별 표현 원칙 (5언어 불릿) | |
| R18 | 01 §6. 언어별 표현 원칙 | |
| R19 | 01 §7. 설계 검토 기준 (6항목 불릿) | |
| R20 | 01 §7. 설계 검토 기준 (exact interface 공개 경계 불릿) | |
| R21 | 01 §8. 검증 요구 (4개 굵은 소제목 아래 9개 불릿) | 옛 §7의 나열 9항목을 "Export와 dependency 방향" / "Signature와 표현" / "완료와 오류" / "메시징과 저장소 등록" 네 갈래로 묶어 재배열. 값 손실 없음(9항목 모두 개별 불릿으로 존재) |
| R22 | 01 §8. 검증 요구 (마지막 문장) | |
| R23 | 03 §1. Framework가 하는 일 | |
| R24 | 03 §1. Framework가 하는 일 | |
| R25 | 03 §2. MeshName·ChannelName·RouteMesh | |
| R26 | 03 §2. MeshName·ChannelName·RouteMesh | |
| R27 | 03 §2. MeshName·ChannelName·RouteMesh | |
| R28 | 03 §2. MeshName·ChannelName·RouteMesh | |
| R29 | 03 §3. 메시지 대상 선택 | |
| R30 | 03 §3. 메시지 대상 선택 | |
| R31 | 03 §4. Logical Multicast와 classic fanout (mermaid 그림 + 불릿) | |
| R32 | 03 §4. Logical Multicast와 classic fanout | |
| R33 | 03 §4. Logical Multicast와 classic fanout | |
| R34 | 03 §5. 실행 owner (4행 표) | |
| R35 | 03 §5. 실행 owner | |
| R36 | 03 §6. 연결 관리 | |
| R37 | 03 §6. 연결 관리 | |
| R38 | 03 §7. Framework가 숨기는 것 | |
| R39 | 03 §7. Framework가 숨기는 것 | |

배치하지 못한 R#: 없음.

## 이동 후 갱신할 링크

`00-foundation` 밖 문서(아직 이동 전, 옛 전역 경로)를 가리키는 링크. 해당 주제가 끝나 새
경로로 옮겨지면 아래 링크도 함께 치환해야 한다.

| 문서 | 링크 | 옛 경로(현재 유효) | 새 경로(예정, 미확정) |
|---|---|---|---|
| `01-public-contract-governance.ko.md` §3 | bindings public/internal 경계 | `../../../../../../../bindings/doc/spec/README.ko.md#공개-vs-내부-api-경계` | 변경 없음(bindings는 이 캠페인 대상 아님) — 상대 깊이만 `00-foundation/` 한 단계 추가로 `../`를 6개→7개로 조정함 |
| `03-overview.ko.md` §6 | ClientServer Channel 상세 | `../09-client-server-channel.ko.md` | `spec/server/<NN-주제>/NN-slug.ko.md` (channel-transport 또는 spot-actor 주제 배치 확정 후) |

## spec-gap 후보

- **G-F1 (mapping §4 S4 관련)** — 옛 `00-public-contract-governance.ko.md` §8 "11.0
  spec-first 정본 규칙"은 이번 재작성 범위 밖으로 두었다. 이 절은 Core service 이관이라는
  특정 마이그레이션의 일회성 전환 정책이며, 나머지 절(영구적인 계약 소유권 규칙)과 성격이
  다르다. 새 `01-public-contract-governance.ko.md`에는 옮기지 않았다 — 옛 문서에만 남아
  있으므로 판정 시 다음을 확인해야 한다.
  - 11.0 마이그레이션이 실제로 끝났는지 (끝났다면 절 자체를 폐기)
  - 끝나지 않았다면 이 정책을 어디로 옮길지 — 실행 ledger, 별도 마이그레이션 문서, 혹은
    `08-layering.ko.md`(구현 스펙, 마이그레이션 이력 허용 안 됨 — 가이드 §4.4 참고) 모두
    적합하지 않을 수 있음
  - 옛 문서의 §8 안에 있는 실제 계약성 문장(예: "Channel·Spot·Actor·STREAM의 기존 public
    symbol, signature와 완료 의미는 그대로 유지한다", "공통 native Framework runtime, private
    C SPI와 service C ABI를 만들지 않는다")은 이미 새 `03-overview.ko.md` §1(R24)이 다른
    표현으로 포함하고 있어 중복 판정이 필요할 수 있다.
  - 코드/스펙을 고치지 않고 대장에만 기록한다.
