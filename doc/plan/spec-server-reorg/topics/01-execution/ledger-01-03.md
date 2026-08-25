# 01-execution — `01-submit-and-completion`·`03-cancellation-and-shutdown` 대장

> [매핑표](mapping.ko.md) §3.2·§3.4·§5.1(R1–R45 중 §1.1–§1.4·§2·§4·§6)·§5.5(R103–R112)의
> 재작성 결과. 산출물: `01-submit-and-completion.ko.md`(490줄), `03-cancellation-and-shutdown.ko.md`(176줄).
> R23–R32(05 §3·§3.1)·R39–R43(05 §5)·R46 이후(33·46·42·41·50)는 이 두 문서의 범위가
> 아니므로 대장에 없다.

## R# → 새 위치

| R# | 새 위치 | 비고 |
|---|---|---|
| R1 | `01-submit-and-completion.ko.md` §1 | naming 적용 범위, grep 확인 |
| R2 | `01-submit-and-completion.ko.md` §2 terminator 표 | needle 2("반환 데이터 없이 완료") 포함 |
| R3 | `01-submit-and-completion.ko.md` §2 | 언어별 terminal 이름 |
| R4 | `01-submit-and-completion.ko.md` §2 표 | `Yield` 제공 범위 |
| R5 | `02-handler-turn-and-execution-gate.ko.md` §3(이 문서 소유 아님) | S8(a) — 이 문서 §2 말미에서 링크만, 본문 미복제 |
| R6 | `02-handler-turn-and-execution-gate.ko.md` §5(이 문서 소유 아님) | S13 — 이 문서 §2 말미에서 "Actor Join은 terminator 대상 아님" 한 문장 + 링크 |
| R7 | `01-submit-and-completion.ko.md` §3 | Worker offload — CPU/I/O 분리 |
| R8 | `01-submit-and-completion.ko.md` §3 | Worker 실패 분류 |
| R9 | `01-submit-and-completion.ko.md` §4 | needle 1("동기 `TrySubmit` 계열을 제공하지 않는다") 포함 |
| R10 | `01-submit-and-completion.ko.md` §5 | Backpressure 규칙, needle 3 포함(`DeadlineExceeded`) |
| R11 | `01-submit-and-completion.ko.md` §5 오류 분류 표 | needle 3·4 모두 포함 |
| R12 | `01-submit-and-completion.ko.md` §5 | Pending admission target |
| R13 | `01-submit-and-completion.ko.md` §6 | Logical Multicast |
| R14 | `01-submit-and-completion.ko.md` §6 | Classic fanout |
| R15 | `01-submit-and-completion.ko.md` §7 표 | Deadline owner |
| R16 | `01-submit-and-completion.ko.md` §7 | Send timeout 값 규칙 |
| R17 | `01-submit-and-completion.ko.md` §7 | STREAM send call별 timeout |
| R18 | `01-submit-and-completion.ko.md` §8 | STREAM reply token |
| R19 | `01-submit-and-completion.ko.md` §9 | 완료 경쟁, mermaid 유지 |
| R20 | `01-submit-and-completion.ko.md` §9 | Timeout budget |
| R21 | `02-handler-turn-and-execution-gate.ko.md` §4(이 문서 소유 아님) | 같은 turn에서의 대기 — §9 안에서 링크 문장만 |
| R22 | `01-submit-and-completion.ko.md` §9 | 늦은 결과 처리 |
| R33 | `03-cancellation-and-shutdown.ko.md` §1 | 협력적 cancellation |
| R34 | `03-cancellation-and-shutdown.ko.md` §2 | Pre-cancelled call, 언어별 표 포함 |
| R35 | `03-cancellation-and-shutdown.ko.md` §3 | 경쟁 처리, mermaid diagram 추가(§7.2) |
| R36 | `03-cancellation-and-shutdown.ko.md` §4 | Logical Multicast cancellation, mermaid diagram 추가 |
| R37 | `03-cancellation-and-shutdown.ko.md` §5 | Relocating/Draining, mermaid diagram 추가 |
| R38 | `03-cancellation-and-shutdown.ko.md` §5 | Draining 후보 제외·drain deadline |
| R44 | `01-submit-and-completion.ko.md` §15 | 언어별 표현 표 |
| R45 | `01-submit-and-completion.ko.md` §15 | C++ `task_t` submit 특례 |
| R103 | `01-submit-and-completion.ko.md` §10 | 완료 자리 하나, atomic take, mermaid 유지 |
| R104 | `01-submit-and-completion.ko.md` §11 | 잠금 밖 실행, 순서 4단계 |
| R105 | `01-submit-and-completion.ko.md` §11 | dispatcher 자리 예약, 4,096 상한(G1 spec-gap 원문 그대로 유지) |
| R106 | `01-submit-and-completion.ko.md` §11 | 예약 실패 시 `CapacityExceeded`, shared lane, exception 격리 |
| R107 | `01-submit-and-completion.ko.md` §10 | `OperationId`/`ReplyRouteId` 표 |
| R108 | `01-submit-and-completion.ko.md` §10 | 등록 후 submit 순서, sequence diagram 유지 |
| R109 | `01-submit-and-completion.ko.md` §12 | 수락 후 재전송 금지, 표 |
| R110 | `01-submit-and-completion.ko.md` §13 | one-way 완료 지점 |
| R111 | `01-submit-and-completion.ko.md` §14 | 문자열 분류 금지 |
| R112 | `01-submit-and-completion.ko.md` §16 | 검증 요구(43 §7 원 목록을 §10–§14 항목으로 재편) |

새 검증 요구 절(둘 다 §7 신설, S7) — 옛 `05-async-execution-policy`·`42-internal-progress-isolation`에는
submit/cancellation 영역 검증 절이 없었으므로 R#이 없는 신규 절이다. 각 문서 §16(submit)·§6(cancellation)의
불릿은 모두 그 문서 안의 기존 규칙(R1–R22·R33–R38·R44·R45·R103–R112)에서 파생했고 새 보장을 추가하지 않았다.

## 코드가 검색하는 문장

`scripts/verify-framework-submit-api.sh --contract`가 `05-async-execution-policy.ko.md`를
읽어 검색하던 4개 문구. 재작성 뒤 새 경로 `01-execution/01-submit-and-completion.ko.md`에서
character-identical로 확인했다(문구 자체는 옮기지 않음 — 스크립트 경로 갱신은 이동 단계 몫).

| 문구 | 새 문서 내 위치 | grep 확인 |
|---|---|---|
| `동기 \`TrySubmit\` 계열을 제공하지 않는다` | §4 One-way submit, 1줄 안에 유지(줄바꿈으로 쪼개지지 않게 재조정) | 확인 |
| `반환 데이터 없이 완료` | §2 terminator 표(one-way 비동기 terminal 행) | 확인 |
| `` `DeadlineExceeded` `` | §3·§5·§7·§8에 여러 번 등장 | 확인 |
| `` `ShuttingDown` `` | §5 오류 분류 표 | 확인 |

```
$ grep -n '동기 `TrySubmit` 계열을 제공하지 않는다' 01-submit-and-completion.ko.md
98:비동기 submit terminator 하나만 제공하고, 동기 `TrySubmit` 계열을 제공하지 않는다.
$ grep -n '반환 데이터 없이 완료' 01-submit-and-completion.ko.md
43:| one-way 비동기 terminal | Source-local admission이 성공하면 반환 데이터 없이 완료하고 ...
445:- One-way call은 admission boundary(§4 표)가 수락하면 반환 데이터 없이 완료하고, ...
$ grep -n '`DeadlineExceeded`' 01-submit-and-completion.ko.md | wc -l
7
$ grep -n '`ShuttingDown`' 01-submit-and-completion.ko.md
154:| runtime이 새 admission을 받지 않음 | `ShuttingDown` |
```

## 이동 후 갱신할 링크

이번 재작성은 `01-execution/` 안에 새 파일 2개만 만들었다. 옛 `05-async-execution-policy.ko.md`·
`43-internal-completion.ko.md`를 참조하던 외부 문서·스크립트는 아직 갱신하지 않는다(README §4.6 —
주제 전체가 끝나고 en과 함께 한 번에 이동). 여기서는 **이번 두 문서가 만든, 아직 완성되지 않은
같은 주제 내부 링크**만 기록한다.

| 새 문서 | 링크 대상 | 상태 |
|---|---|---|
| `01-submit-and-completion.ko.md` §제목 아래·본문 끝 | `README.ko.md`(주제 목차) | 아직 없음 — README 작성 시 존재 |
| `01-submit-and-completion.ko.md` §제목 아래·본문 끝·§2·§9 | `02-handler-turn-and-execution-gate.ko.md`(전체 링크 + `#3-yield-시-gate와-claim`·`#5-actor-join과-defer-완료-경계`·`#4-같은-turn에서의-대기와-반납`) | 아직 없음 — `#4-같은-turn...` anchor는 README §3.2 content rules가 문자 그대로 지정한 값을 그대로 씀. `#3-...`·`#5-...`는 mapping §3.3의 절 제목("`Yield` 시 gate와 claim", "Actor Join과 `Defer()` 완료 경계")에서 이 저장소 slugify 규칙(공백→`-`, backtick·괄호 제거, 대문자→소문자)으로 도출. 02 작성 뒤 `check_doc_links.py`로 재확인 필요 |
| `01-submit-and-completion.ko.md` §1 개요 | `05-application-job-queue-and-backpressure.ko.md` | 아직 없음 — 주제 README 완성 후 존재 |
| `03-cancellation-and-shutdown.ko.md` §제목 아래·본문 끝 | `README.ko.md`(주제 목차)·`02-handler-turn-and-execution-gate.ko.md`(nav의 "이전") | 아직 없음 |
| 기존 파일(변경 안 함) | `04-session/01-stream-session.ko.md` §9, `04-session/02-session-actor-binding.ko.md` §10 이관 pointer | mapping §3.6.1 — `05-application-job-queue-and-backpressure.ko.md` 작성 담당 몫, 이 두 문서와 무관 |

`check_doc_links.py framework`를 이 두 파일에 대해 실행한 결과, 위 표의 "아직 없음" 항목만
"링크 대상 없음"으로 보고되고 그 밖의 모든 링크(용어집 10개 anchor, `06-framework-api.ko.md#12-...`,
`29-transport-liveness.ko.md#5-...`, `00-foundation/07-framework-error-model.ko.md`, 언어별
exact interface 문서)는 통과했다.

## spec-gap 후보

이 두 문서 범위에서 새로 발견한 gap은 없다. mapping §4·spec-gap 표의 기존 후보 중 이 문서가
그대로 옮긴 것만 재확인한다.

| # | 위치 | 내용 |
|---|---|---|
| G1(기존, mapping §"spec-gap 후보") | `01-submit-and-completion.ko.md` §11(옛 43 §2) | "진행 중 operation과 dispatcher에서 대기·실행 중인 callback을 합친 수는 4,096개를 넘지 않는다"의 적용 범위(process 전체/host instance 단위/언어별 차이 여부)를 문서가 밝히지 않는다. 원문 그대로 옮기고 새 보장을 만들지 않았다. 4언어 구현 대조에서 실제 적용 단위를 근거로 보고받아 결정 대기로 올릴 항목 |

## 완료 점검

- `grep -n ' $'` — 두 파일 모두 0건.
- `grep -c '^\*\*결정'` — 두 파일 모두 0건(S5, 굵은 규칙 문장 + 이유 불릿으로 전환).
- 4개 needle 모두 `01-submit-and-completion.ko.md`에서 문자 그대로 확인.
- 용어집 anchor 10개(`owner`·`ready`·`deadline`·`meshnode`·`routemesh`·`channelname`·
  `logical-multicast`·`classic-fanout`·`reply-token`·`drain-deadline`) 전부 실재 확인.
- `01-submit-and-completion.ko.md` 490줄, `03-cancellation-and-shutdown.ko.md` 176줄.
- 배치 못 한 R#: 없음(R5·R6·R21은 mapping이 명시적으로 다른 문서 소유로 지정한 대로
  링크만 남기고 그 문서 몫으로 표시했다 — 02-handler-turn-and-execution-gate.ko.md 작성 시
  그 대장에서 "새 위치"를 채워야 완결된다).

---

[매핑표](mapping.ko.md) · [캠페인 지침](../../README.ko.md)
