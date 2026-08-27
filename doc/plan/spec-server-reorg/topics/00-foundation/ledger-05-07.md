# 규칙 등가성 대장 — 05-message-model / 07-framework-error-model

> [매핑표](mapping.ko.md) §5의 `message-model`(R81~R107)과 `framework-error-model`
> (R241~R266) 행을 재작성한 두 문서의 절 번호로 채운 것. "새 위치" 열은 새 문서에서 grep으로
> 핵심 문구를 실물 확인한 뒤에만 채웠다.

## R81–R107 — `05-message-model.ko.md`

| R# | 새 위치 (절 번호·제목) | 비고 |
|---|---|---|
| R81 | 개요 문단(H1 아래, `## 1.` 앞) | envelope·multipart encoding 공통 wire schema 문장 |
| R82 | 1. Typed 메시지 | |
| R83 | 1. Typed 메시지 | |
| R84 | 2. 메시지 종류와 완료 (표) | |
| R85 | 2. 메시지 종류와 완료 | |
| R86 | 2. 메시지 종류와 완료 | Object creation request 문단 |
| R87 | 3. MessageContext | |
| R88 | 3. MessageContext | |
| R89 | 3. MessageContext | |
| R90 | 3. MessageContext | |
| R91 | 3. MessageContext | |
| R92 | 4. Global object reference JSON | |
| R93 | 4. Global object reference JSON | JSON 예시 코드블록 2개 |
| R94 | 4. Global object reference JSON | |
| R95 | 5. framework-json-v1 typed payload profile | 11항목 불릿 |
| R96 | 5. framework-json-v1 typed payload profile | |
| R97 | 5. framework-json-v1 typed payload profile | |
| R98 | 6. Application metadata (표) | |
| R99 | 6. Application metadata | |
| R100 | 7. 전달 규칙 (표) | |
| R101 | 7. 전달 규칙 | trace 정보는 `27-flow-correlation`(observability 주제, 미이동) 링크 |
| R102 | 8. Ownership과 크기 제한 | |
| R103 | 8. Ownership과 크기 제한 | |
| R104 | 8. Ownership과 크기 제한 | |
| R105 | 8. Ownership과 크기 제한 | |
| R106 | 8. Ownership과 크기 제한 | |
| R107 | 8. Ownership과 크기 제한 | `12-spot-messaging`(spot-actor 주제, 미이동) 링크만 |

## R241–R266 — `07-framework-error-model.ko.md`

| R# | 새 위치 (절 번호·제목) | 비고 |
|---|---|---|
| R241 | 1. 범위 | |
| R242 | 1. 범위 | |
| R243 | 2. 공통 ErrorKind (표 13행) | |
| R244 | 2. 공통 ErrorKind | |
| R245 | 3. 호출 전에 확인할 수 있는 오류 | |
| R246 | 3. 호출 전에 확인할 수 있는 오류 | |
| R247 | 4. Send 완료와 실패 | |
| R248 | 4. Send 완료와 실패 (표 4행) | |
| R249 | 4. Send 완료와 실패 | |
| R250 | 5. Request 완료와 실패 | |
| R251 | 5. Request 완료와 실패 | `CapacityExceeded` 굵은 규칙 문장 |
| R252 | 5. Request 완료와 실패 | `Unavailable` 굵은 규칙 문장 |
| R253 | 5. Request 완료와 실패 | "이 구분은 대기열에만 적용한다" 굵은 규칙 문장 |
| R254 | 5. Request 완료와 실패 | Message Follow relay queue·relocation ingress hold 굵은 규칙 문장 |
| R255 | 5. Request 완료와 실패 | |
| R256 | 5. Request 완료와 실패 | |
| R257 | 6. Typed 결과와 Rejected | |
| R258 | 6. Typed 결과와 Rejected | |
| R259 | 7. 재시도 판단 | |
| R260 | 7. 재시도 판단 | |
| R261 | 7. 재시도 판단 | |
| R262 | 2. 공통 ErrorKind (마지막 문단) | 언어별 투영 범위 규칙이므로 §9 검증이 아니라 §2가 소유하도록 재배치 — R241~R266 옛 번호 순서와 다르나 절 순서는 가이드 §4.3(요약·상세·검증 분리)을 따름 |
| R263 | 9. 검증 요구 | 5개 굵은 소제목 불릿으로 재구성(§9.3) |
| R264 | 8. Application job queue 포화 | |
| R265 | 8. Application job queue 포화 | |
| R266 | 8. Application job queue 포화 | |

## 이동 후 갱신할 링크

두 문서 모두 아직 `00-foundation`으로 이동하지 않은 다른 주제의 옛 경로를 링크한다. 이동은
캠페인 마지막 단계(§5)에서 en과 함께 한 번에 처리하므로, 그때 아래 링크를 새 경로로 치환해야
한다.

| 문서 | 절 | 링크 | 새 경로(추정) |
|---|---|---|---|
| `05-message-model.ko.md` | 7. 전달 규칙 | `../27-flow-correlation.ko.md` | observability 주제 문서로 이동 예정 |
| `05-message-model.ko.md` | 8. Ownership과 크기 제한 | `../12-spot-messaging.ko.md` | spot-actor 주제 문서로 이동 예정 |
| `07-framework-error-model.ko.md` | 5. Request 완료와 실패 (×2) | `../15-spot-actor.ko.md` | spot-actor 주제 문서로 이동 예정 |
| `07-framework-error-model.ko.md` | 5. Request 완료와 실패 | `../16-spot-address-messaging.ko.md` | spot-actor 주제 문서로 이동 예정 |
| `07-framework-error-model.ko.md` | 5. Request 완료와 실패 | `../21-location-runtime.ko.md` | location-relocation 주제 문서로 이동 예정 |

같은 주제 안(00-foundation) 링크는 `02-glossary.ko.md#anchor`, `04-interaction-model.ko.md`,
`06-framework-api.ko.md`, `08-layering.ko.md`로 이미 최종 경로를 사용했다.

## spec-gap 후보

이번 재작성에서는 새로 발견한 spec gap 후보가 없다. `05-message-model`과
`07-framework-error-model`의 옛 문서(04, 32)는 매핑표 §4의 S5(메시지 완료 표 중복)를 제외하면
구조 문제가 보고되지 않았고, S5는 이번 재작성에서 절 소유를 나눠 해소했다(§2는 메시지 종류·완료,
대상 선택은 `04-interaction-model`이 소유).
