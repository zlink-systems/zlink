# 2026-09-06 스펙 변경 상세 (D-101, D-104~D-108)

모든 항목은 행동 변경 없음(구현이 이미 그렇게 동작하거나, 문서끼리 충돌하던 것을 하나로 정리). 커밋: `39e270a602`, `02dc4f6a33`, `eea1309248`, `7627284944`, `020c4ea99c`, `325cb2e861`. ko/en 동일.

## 1. 결정 승격 (D-101)
| 위치 | 변경 | 이유 |
|---|---|---|
| glossary `Connection intent` 신설 | peer당 하나, application 구성/auto-connect 소유자가 만들고 제거, 제거는 terminal | MeshNode·Actor 문서가 같은 개념을 이름 없이 쓰고 있었음(D-093/D-100) |
| mesh-node §7.1 | intent 규칙 3개: (1) outbound intent 제거 = binding disconnect, 판정 한 곳 (2) 제거된 intent는 늦은 READY로 되살아나지 않음 (3) intent 제거+admitted peer 부재 = 그 peer의 lifecycle 종료 | D-100, D-098-8, D-093 |
| actor-model §8.1 | durable operation 종료 규칙: lifecycle 종료 신호 하나, deadline 소유자 operation 하나 | D-093(ZoneWorld G4) |
| actor-model §9 | 관찰 항목 2개(intent 제거 후 `Unavailable`, wrapper가 먼저 timeout 안 함) | 검증 요구는 인터페이스 관찰만 |
| liveness §2 | 경과 시간·deadline·retention은 monotonic clock 하나; wall clock은 표기 전용 | D-095(WSL 클록 점프) |
| host-relocation §14 step 2 | Draining 게시는 terminal만 기다림, 전파 대기 없음 | D-098-4 |
| ZMP §4.1 | 같은 RID 동시 count-2 attempt는 wire에서 구분 불가 → lane 중복 규칙으로 닫고 intent 재시도; **intent가 하나일 때 수렴**(과잉 주장 수정) | D-096, R2-B1 |
| socket README `zlink_unbind`/`zlink_close` + §8 | transport 불문 반환 전 endpoint 해제, 즉시 rebind 가능 | D-098-1 |

## 2. Core protocol·systems (D-104)
- **auto-hwm**: WRITABLE wake 조건은 socket README REQUEST 절만 소유(참조로); inproc cap 9행 표 → "유한 manual 있으면 최솟값 / auto 있으면 water-filling / 둘 다 unlimited" 3행; HWM 축소는 writer admission 즉시·snapshot applied는 drain 뒤(코드 `pipe.cpp:947`).
- **connection-memory**: frame 회계 산식·검증을 auto-hwm 참조로(3중 정의 제거).
- **thread-safety/threading-model**: close가 "기다리거나 BUSY"라는 문장 삭제 — 코드는 fail-fast EBUSY.
- **ZMP §9**: "admission 전 payload pool" 문장 삭제(`PENDING_MAX_*`는 ABI 보존 전용); §10 READY metadata 조건을 하나로; 비 DEALER·ROUTER가 lane property를 받으면 protocol failure(ko/en 동일), STREAM은 RAW이므로 제외.
- **RAW §4**: "0 byte payload는 제어 이벤트"를 transport 입력에 한정; PACKET의 `0+0` packet은 유효 데이터(STREAM 계약).
- **io-thread §3.4**: listener·command owner는 least-load, transport session은 round-robin, STREAM은 RR/minload 환경변수(astra 제안은 least-load 사용처를 놓쳤음); §6은 구현 서술이므로 contract test 대신 내부 확인 조건.
- **hot-path §4/§5**: DONTWAIT token 발급 여부는 socket README가 소유(hot path는 비용 경계만); gate 의무는 §5.1 매 변경, §5.2는 release 준비 단계(CONTRIBUTING ko/en 정합).
- **socket README §4 RID 정책**: REJECT/HANDOVER는 **같은 방향** 중복에만 적용, 반대 방향 충돌은 정책과 무관하게 RID 비교로 방향 선택 + 패자 standby. astra는 코드의 우회를 결함이라 했으나, framework가 기본 REJECT에서 이 경로로 양방향 connect를 수렴시키므로 스펙을 코드에 맞춤.

## 3. Framework spot·actor (D-105)
- **spot-model §3.4**: closing reason 표에 값(0~3) 열 추가, 소유자 확정; membership §5의 3행 표(IdleEvicted 누락) 삭제 → 참조.
- **membership §2 step 7**: callback exception·timeout은 target의 Abort가 Failed publish; node 종료로 남은 reservation은 recovery cleanup Abort(terminal record 없음) — 같은 절 안의 두 문장 충돌 해소.
- **membership §4**: 굵은 불변조건이 뒤집혀 있었음("accepted 전 실패하면 복원 안 함") → "accepted 전 명시적 실패만 복원, accepted 뒤에는 복원 안 함".
- **actor-model §3.1**: "crash recovery를 위해 저장" 문장 삭제(§3.1 뒤의 process-local 수명과 모순).
- **actor-model §8.1**: replay 규칙 3개(주체·대상, 조건 = typed transient transport 실패만, attempt는 남은 deadline 전부·횟수 제한 없음)를 굵은 규칙+이유로; §9는 관찰 4개로 축소.
- **routing §2.6 검증 / address messaging 오류 표**: direct message는 ObjectGeneration 비교 안 함(owner generation·lease만), generation 불일치 오류는 ActorRef/SpotRef control에 한정.
- **mesh-node §7.1 세 번째 규칙**: lifecycle 사실만 두고 소비 방법은 Actor §8.1 참조.

## 4. Core API (D-106)
- **dealer/router request timeout 문단·검증**: "disconnect 뒤 correlation 유지" 잔존 문장 → submit 시점 pair 종료 시 즉시 `NOT_CONNECTED`(D-090).
- **README part send**: 대기 토큰은 "그 제출을 거절한 자원이 회복될 때" 깨어남(SEND/physical: write credit, correlation 거절: reservation 반환); polling은 참조.
- **XPUB**: 실패 경계는 PUB §3 소유(pre-submit 실패는 sequence 유지); topic buffer 부족은 `BUFFER_TOO_SMALL/ENOBUFS`·event 보존(문서의 EMSGSIZE·유실 서술은 코드와 달랐음); RID view 수명은 socket별(thread별 공유 아님).
- **SUB/XSUB**: "capacity 0이면 실패" 특례 삭제(길이 0 topic은 capacity 0으로 성공).
- **PAIR/PUB/XPUB**: 소비된 part는 초기화된 빈 message(재초기화 불필요; `consume_send_frame` close+init).
- **token 종료 조건(README/stream/dealer/router)**: socket close·context 종료는 토큰을 내부에서 끝내고 record를 전달하지 않음(코드: close가 ready queue 폐기).
- **receive owner 충돌**: `ZLINK_RECV_BUSY`/EBUSY(03-errors 대응표가 단일 소유자; 코드 `from_errno(EBUSY)`); astra는 INVALID_STATE 유지를 원했으나 예외 규칙을 없애는 쪽 선택.
- **02-message**: 실패한 part 호출은 전달된 part만 소비, 미제출 part 불변.
- **08-runtime-boundary**: 옛 callback metadata queue 서술 → completion record가 reply payload 소유(README 참조).
- **README option**: discovery handle 잔존 문장 삭제.
- **06-monitoring**: lock 문장은 §6.3만, 검증 절은 일관성 관찰로; D-092 관찰 항목(수신 drain 없이 DISCONNECTED) 추가.
- **README §8**: unbind/close rebind 항목을 "지원하는 각 transport"로; stream.en bullet 결합 복구.

## 5. Relocation·observability (D-107)
- **routing §2.6 표**: owner unavailable 정책의 소유자 = 장애 정책 §4.2(링크).
- **runtime-monitoring §5**: fanout ready 판정은 liveness §4 소유, status는 투영.
- **relocation-flow §9**: reconciliation 문단 수정 — Store가 source를 owner로 보여도 dispatch를 다시 열지 않음; target commit이 보이면 route 채택, 아니면 `Unavailable`(cpp 구현이 이 규칙 위반 → 수정 job).
- **location-runtime §10 / 장애 정책 §7**: StoreFailureGrace 동안 마지막 desired 목록의 intent 유지(미연결 target 포함), 목록 밖 새 target 금지 — 4언어 코드에 맞춤(astra의 "grace 중 connect 금지" 제안 기각).
- **host-relocation §16**: "terminal event 무유실" → monitoring §7.2의 bounded 보관 규칙 참조(4언어 모두 bounded).
- **metrics §12 / tracing §7**: 순회·allocation·호출부 검사 조건을 규칙 문단의 내부 확인 조건으로.

## 6. Transport·session (D-108)
- **liveness §3**: service ready 정의·관찰의 소유자 선언(topology·ClientServer·wire·MeshNode는 참조).
- **topology §4 / liveness §2**: pair 생략 조건을 "둘 다 Object Client **이고** Server membership 없음(weight 0 포함)"으로(축약 문장이 구현과 달랐음).
- **wire §5**: 5초/15초·ACK 판정 규칙을 liveness 참조로, wire는 schema·epoch만.
- **wire §3.2**: 100 ms 종료 시점 → session binding §14 참조.
- **liveness §10**: 수신 상한 검증을 본문(건수 64 고정, byte·시간 재량)과 정합.
