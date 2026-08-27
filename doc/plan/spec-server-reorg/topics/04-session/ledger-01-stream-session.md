# 규칙 등가성 대장 — stream-session

`framework/doc/framework/common/spec/server/session/stream-session.ko.md` 재작성 결과.
R1–R17만 다룬다(R18 이후는 `session-actor-binding` 문서 담당).

| R# | 새 위치 (절 번호·제목) | 비고 |
|---|---|---|
| R1 | §2 역할과 책임 (결정 문단) | §10 검증 요구에 "Core packet/raw callback 미사용" 행(정적 검사) 추가 |
| R2 | §4 연결 수락부터 session callback까지 (결정 문단 2개로 분리) | 결정(admission 실패 시 read 중단)과 계약(폐기·재전달 없음)을 별도 문장으로 분리. §10에 admission 실패 시 read 중단 관찰 행(contract test) 추가 |
| R3 | §4 연결 수락부터 session callback까지 | handler filter 미적용 |
| R4 | §4 연결 수락부터 session callback까지 | routing ID 손실 없이 전달 |
| R5 | §5 Reply 상관관계 | |
| R6 | §4.1 Transport 종료 경계 | 지정 목차상 §7이 아니라 §4.1에 위치(요구된 목차 구조 우선) |
| R7 | §6 Payload 변환과 codec 경계 | |
| R8 | §7 오류 경계 | |
| R9 | §7 오류 경계 | |
| R10 | §3 등록과 startup 검증 | |
| R11 | §9 수치와 제한 (표 + 산문 2문단) | §3은 .NET 코드 발췌 + 인라인 주석만 두고, `MaxMessageSize`의 값·범위는 표 한 행에, `0`/`-1`/음수·양수 규칙과 초과 시 동작(EMSGSIZE·종료·client 관찰)은 표 아래 산문 문단으로 분리 |
| R12 | §9 수치와 제한 (표) | 수치가 아니므로 표에 넣지 않고 §3.2 startup 실패 조건 행("한 node에 session을 둘 이상 등록했다")과 §3 "등록 표면의 축" 표에만 서술. §9 표는 `MaxMessageSize` 한 행만 유지 |
| R13 | §3.1 TLS | 수치가 아니므로 §9로 보내지 않고 §3.1에 "기본값은 요구하지 않는 것이며"로 인라인 서술(cert+key 필수·검증 실패 거부·client 쪽 transport 결정과 같은 절) |
| R14 | §3.2 Startup 검증 (표) | 8개 조건 모두 유지 |
| R15 | §8 Session에서 Actor로 | |
| R16 | §8 Session에서 Actor로 | 비노출 목록(Node RID·binding generation·authority fence·codec)을 한 문장으로 압축, 세부 command별 전달 계약은 `session-actor-binding.ko.md`로 링크 |
| R17 | §8 Session에서 Actor로 | |

## 이관 처리 (참고, R번호 없음)

- 옛 §8 문단 3–5(physical disconnect 자동 통지, 100 ms 교체 close, relocation/seal/command 44/3,000 ms 절차)는 이번 문서에서 삭제하고 `session-actor-binding.ko.md` §6·§8로 이관(사이드카 문서 소관, R34/R42–R48 등).
- 옛 §10 "Session dispatch와 shared permit"은 execution 주제로 이관. 새 문서 §9 끝에 이관 링크 문장만 남김(현재 정본: `../33-core-hwm-application-job-flow.ko.md`).

## spec-gap 후보

1. **R12(node당 session type 1개)와 R14의 "같은 session type을 중복 등록했다" 행의 관계가 불명확.** R12는 한 node에 session type을 하나만 등록할 수 있다고 정하는데, R14 표는 "같은 session type을 중복 등록했다"를 "한 node에 session을 둘 이상 등록했다"와 별도 행으로 나열한다. 한 node에 session이 하나만 허용된다면(R12) "같은 node에 같은 type을 두 번 등록"은 "둘 이상 등록" 행에 이미 포함되므로, "같은 session type 중복 등록" 행이 가리키는 시나리오(서로 다른 node에 같은 session type을 등록하는 경우인지, 같은 node 안에서의 경우인지)가 원문에 명시돼 있지 않다. 새 문서는 두 행을 원문 그대로 §3.2 표에 남겨 두었다. (원문 위치: `framework/doc/framework/common/spec/server/04-session/01-stream-session.ko.md` §7.2 표)
