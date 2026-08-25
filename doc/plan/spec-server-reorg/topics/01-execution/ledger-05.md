# 규칙 등가성 대장 — 05-application-job-queue-and-backpressure.ko.md

> [mapping.ko.md](mapping.ko.md) §5.2(R46–R71)·§5.3(R72–R87 중 R72–R75·R78·R79·R83·R85–R87)·
> §5.4(R88–R102 중 R93–R97·R102)·§5.8(R156–R158)의 새 위치를 기록한다.
> 대상 문서: `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md`.
>
> 이 문서가 **가져오지 않은** 옛 규칙 — R76·R77·R78(wakeup 일반)·R80–R82는
> `02-handler-turn-and-execution-gate.ko.md`·`04-spot-timer.ko.md`가 소유한다. R88–R92·
> R98–R100은 `02-handler-turn-and-execution-gate.ko.md`가 소유한다. 이 표에 없는 46/42의
> R#는 누락이 아니라 다른 문서 소유다.

## `33-core-hwm-application-job-flow` (R46–R71)

| R# | 새 위치 | 비고 |
|---|---|---|
| R46 | §1 두 독립된 capacity authority | 두 authority가 서로 다른 과부하를 관찰, 설정·profile·단위·계상경계·관측값 미공유 |
| R47 | §1 두 독립된 capacity authority | Core HWM=마지막 안전장치, Application job queue=유입 속도 안전장치, 둘 다 필요하나 같은 보호 아님 |
| R48 | §1 두 독립된 capacity authority(표) | 두 authority 표 — 제한 대상·계상/획득 시점·반환 시점 |
| R49 | §1 두 독립된 capacity authority | Core frame charge = payload+metadata, binding 넘김 시 charge 종료, retained-credit lease 미사용 |
| R50 | §1 두 독립된 capacity authority | receive 뒤 payload storage는 06 문서 일반 규칙, 복사·이동·해제는 HWM credit·별도 authority 아님 |
| R51 | §1 두 독립된 capacity authority | pressure count 공식(reserved+queued), waiter 미포함, 1:N callback당 permit 1개 |
| R52 | §2 설정과 profile 경계(표) | 설정군 표 — Core/Framework host 소유, 기본값 `Balanced` |
| R53 | §2 설정과 profile 경계 | startup에 Core 설정 전달만, 비율 계산·connection 수 나누기 없음, snapshot은 읽기전용 |
| R54 | §2 설정과 profile 경계 | Framework feedback = RUNNING/PAUSED 절대 상태 하나뿐 |
| R55 | §3 Ordinary ingress permit 순서 | 5단계 순서(다이어그램 포함) |
| R56 | §3 Ordinary ingress permit 순서 | handler 시작 후 await·suspension은 재획득 없음, relocation backlog handoff 후 재획득 |
| R57 | §3 Ordinary ingress permit 순서 | pre-receive terminal만 우회, 소급 우회 없음, source별 waiter 1개·oldest 순·batch tail 이동 |
| R58 | §3 Ordinary ingress permit 순서 | heartbeat·topology·relocation·SendReady kind 12는 completion supply 아님 |
| R59 | §3 Ordinary ingress permit 순서(금지 목록) | 금지 4항 |
| R60 | §6 Pressure 상태와 socket 제어 | pause/resume 공식, `P`=80 기본, `R`=60 기본, 경계 사이 유지 |
| R61 | §6 Pressure 상태와 socket 제어 | RouteMesh·ClientServer paired socket에만 적용, PUB/SUB·fanout·STREAM 제외 |
| R62 | §6 Pressure 상태와 socket 제어 | PAUSED는 Core HWM 불변, RUNNING은 remote-pause만 제거, pressure는 route ready·liveness 불변 |
| R63 | §6 Pressure 상태와 socket 제어 | host queue owner가 sync 경계에서 계산, 중복 적용 없음, stale transition 무시 |
| R64 | §6 Pressure 상태와 socket 제어(내부 확인 조건) | 새 socket 적용 후 registry 게시, close는 registry 제거 먼저, lock 밖 binding 호출 |
| R65 | §6 Pressure 상태와 socket 제어 | receive-flow state API가 유일한 runtime 제어 지점, raw frame·범용 control lane 미사용 |
| R66 | §7 Send completion과의 합성 | Core/binding이 HWM 대기·재시도·completion 소유, Framework는 operation 1개만 시작 |
| R67 | §7 Send completion과의 합성 | send_ready callback·waiter·retry adapter 없음, SendReady kind 12는 별도 계약 |
| R68 | §9 큰 payload와 운영값 | job count 제한, byte 비가중, byte hard cap 아님 |
| R69 | §9 큰 payload와 운영값 | `MaxQueuedApplicationJobs` 조정 기준, `MaxMessageSize` 별도, profile 연결·lease 복구로 해결 안 함 |
| R70 | §9 큰 payload와 운영값 | Core HWM은 계속 마지막 안전장치, local queue byte 적체 시 TCP backpressure |
| R71 | §10 검증 요구(전 항목에 분산) | contract test 8항 — 아래 대응표 참고 |

## `46-internal-dispatch-loop` (R72–R87 중 이 문서가 가져온 행)

| R# | 새 위치 | 비고 |
|---|---|---|
| R72 | §3 "준비된 owner 집합 (구현)" | missed wakeup 없음, 중복 등재 없음 — 규칙 문장은 본문, 내부 확인 조건은 별도 문단 |
| R73 | §3 "넣을지 판단하는 것과 넣는 것을 쪼개지 않는다 (구현)" | 6단계 commit, 다이어그램은 46의 flowchart를 옮기지 않고 산문+번호 목록으로 흡수 |
| R74 | §3 "넣을지 판단하는 것과 넣는 것을 쪼개지 않는다 (구현)" | 확인 실패 message 대기열 미등장, 실패해도 건수·byte·sequence 불변 |
| R75 | §3 "넣을지 판단하는 것과 넣는 것을 쪼개지 않는다 (구현)" | **언어별 재량** — 잠금 방식 자유, 판정 기준 = 확인+넣기만 구간 안, 역직렬화·handler 조회는 밖 |
| R79 | §4 소켓에서 여러 건 읽기 (구현) | 건수·byte·경과시간 중 먼저 닿는 한도, cursor 유지, 모든 multi-connection 경로 |
| R83 | §5 수신 처리와 상태 변경 분리 (구현) | 소유권 이전 후 즉시 반환, 형식검사 handler 전에 완료, ProtocolError/기록만 분기 |
| R84 | §10 검증 요구(일부) + §3/§5 내부 확인 조건(일부) | 46§9 19항 중 이 문서 소유분 — 아래 대응표 참고 |
| R85 | §3 (33 §4와 중복 — 통합 흡수, 별도 본문 없음) | source waiter·oldest 순·batch tail 이동 세부는 §3 "Pre-receive에..." 문단에 흡수 |
| R86 | §3 "Permit 반환과 대기 중 자원 점유 금지 (구현)" | exact-target 첫 instruction 반환, cancel/close/shutdown 1회 정리, 자원 쥔 채 재획득 대기 금지 |
| R87 | §6 Pressure 상태와 socket 제어 (33 §5·§6과 중복 — 통합 흡수) | reserved→queued 전이, running/paused 조건 동일, shutdown 무기한 대기 안 함 |

이 문서가 **가져오지 않은** 46의 R# — R76(owner 배타권·fencing), R77(시간 예산·batch 처리),
R78(작업 도착 즉시 wakeup 일반), R80–R82(timer 자원·늦은 tick·tick 실행권한)는
`02-handler-turn-and-execution-gate.ko.md`(R76·R77)와 `04-spot-timer.ko.md`(R80–R82)가
소유한다. R78은 두 문서 어디에도 전용 절이 없다 — mapping 표 §3(새 구조)의 "46 §5(일부)"는
독립 절이 아니라 §4(46§6)의 wiring 제외 문장(RouteMesh socket option은 channel-transport
소유)으로 흡수됐다.

## `42-internal-progress-isolation` (R88–R102 중 이 문서가 가져온 행)

| R# | 새 위치 | 비고 |
|---|---|---|
| R93 | §8 Backpressure 3단계와 한도 종류 | 3단계는 send/publish/one-way만, Request는 CapacityExceeded/Unavailable |
| R94 | §8 Backpressure 3단계와 한도 종류 | 미확정 구간에만 적용, 실행권한 안 쥠, 대기 자리 한도, `Backpressured` 비-public |
| R95 | §3 (33 §4/46 tail과 중복 — 통합 흡수, 별도 본문 없음) | ordinary source permit readiness, control/malformed 동일 처리는 §3에 이미 있음 |
| R96 | §8 Backpressure 3단계와 한도 종류 | StreamNode `MaxMessageSize` 독립 wire guard, 기본 64 KiB, server→client 미적용 |
| R97 | §8 Backpressure 3단계와 한도 종류(표) | 한도 4종 표 — 무엇으로 재는가·포화 의미 |
| R101 | §10 검증 요구(일부) | 42§8 14항 중 이 문서 소유분(5·6·7·8·13·14항) — 아래 대응표 참고 |
| R102 | §6 Pressure 상태와 socket 제어 (33 §5·§6, 46 tail과 3중 중복 — 통합 흡수) | 동일 내용, 별도 본문 없음 |

이 문서가 **가져오지 않은** 42의 R# — R88–R92(application/infrastructure 영역 분리, 자원
배분, 관측 비점유), R98–R100(실행 영역 문맥 표시, owner당 두 FIFO)는
`02-handler-turn-and-execution-gate.ko.md`가 소유한다.

## 이관 — session 파일럿의 shared permit 규칙 (R156–R158)

| R# | 새 위치 | 비고 |
|---|---|---|
| R156 | §3 Ordinary ingress permit 순서(도입부 + "Permit 반환과 대기 중 자원 점유 금지") | 옛 `19-stream-session.ko.md` §10 유래. §3 도입부가 STREAM application packet을 명시적으로 열거하고, 두 세션 문서를 링크해 "이 규칙이 다루는 문맥"으로 지목한다. 개별 필드 나열은 반복하지 않는다 |
| R157 | §3 Ordinary ingress permit 순서(도입부) | 옛 `48-internal-session-binding.ko.md` 말미 유래. §3 도입부가 cross-node Session application record를 명시적으로 열거한다 |
| R158 | §3 Ordinary ingress permit 순서 전체 | 옛 `05-async-execution-policy.ko.md` §10(이 주제 자신의 사본) 유래. 5단계 순서·pre-receive 우회·release 시점·capacity wait 규칙이 모두 §3 본문에 있다 |

R156·R157·R158은 하나의 계약 문장("Ordinary ingress는 문맥과 무관하게 같은 순서를 따른다")
으로 통합됐고, 세 옛 문서의 구체 문구는 §3 어디에도 각각 따로 다시 쓰지 않았다. §3만 읽고도
(a) handshake·bind·unbind가 receive/claim 전에 permit이 필요하다는 것, (b) application
packet의 반환 시점이 exact-target callback의 실제 첫 instruction이며 session callback
시작도 같은 지점이라는 것, (c) batch·1:N이 확보한 permit보다 많은 job을 게시하지 않는다는
것 세 가지를 모두 확인할 수 있다.

## 흡수한 중복 위치

`05-application-job-queue-and-backpressure.ko.md`가 단독 소유하는 두 규칙 군의 통합 이력이다.

### Ordinary ingress permit 순서 (§3) — 7곳 흡수

| # | 옛 위치 | 내용 |
|---|---|---|
| 1 | `33-core-hwm-application-job-flow.ko.md` §4 | 원 소유 — 5단계 순서, pre-receive 우회, 금지 4항 |
| 2 | `46-internal-dispatch-loop.ko.md` 말미 "Shared supply permit, readiness와 fairness" | source waiter·oldest 순·batch tail 이동, 반환 시점, 자원 쥔 채 재획득 대기 금지 |
| 3 | `42-internal-progress-isolation.ko.md` §5 말미 문단("Ordinary source는 host-shared…") | ordinary source permit readiness, control/malformed 동일 처리, receive 뒤 payload owner가 Core HWM budget 미점유 |
| 4 | `42-internal-progress-isolation.ko.md` 말미 "포화 상태의 progress 분리" | terminal supply만 우회, permit 순서 소유는 46, payload storage 수명은 50(→06) |
| 5 | `05-async-execution-policy.ko.md` §10 "Application job queue 비동기 경계" | 이 주제 자신의 사본 — 5단계 요약, release 시점, capacity wait |
| 6 | 옛 `19-stream-session.ko.md` §10 "Session dispatch와 shared permit"(현재 `04-session/01-stream-session.ko.md` §9 말미 이관 pointer) | STREAM application packet 문맥의 같은 규칙 |
| 7 | 옛 `48-internal-session-binding.ko.md` 말미 "Session control permit"(현재 `04-session/02-session-actor-binding.ko.md` §10 말미 이관 pointer) | cross-node Session application record 문맥의 같은 규칙 |

### Pressure 상태와 socket 제어 (§6) — 3곳 흡수

| # | 옛 위치 | 내용 |
|---|---|---|
| 1 | `33-core-hwm-application-job-flow.ko.md` §5·§6 | 원 소유 — pause/resume 공식, paired socket 적용 범위, socket lifecycle |
| 2 | `46-internal-dispatch-loop.ko.md` 말미 "Permit 변경과 pressure 평가" | 동일 내용을 permit 변경 관점에서 재서술 |
| 3 | `42-internal-progress-isolation.ko.md` 말미 "Pressure 전이와 송신 완료" | 동일 내용을 infrastructure domain 관점에서 재서술 |

## R84(46§9)·R101(42§8) 세부 대응 — 옛 검증 항목 → 새 위치

### 46 §9 "확인할 결과" 19항

| # | 옛 항목(요약) | 새 위치 |
|---|---|---|
| 1 | message 연속 도착 시 깨우기 횟수 < message 수 | `02-handler-turn-and-execution-gate.ko.md` 소유(wakeup 일반, R78) |
| 2 | 확인 통과 못한 message가 대기열에 미등장 | §10 "확인에 실패한 send·request는 owner queue의 건수·byte·sequence 관측값을 바꾸지 않는다" |
| 3 | 확인시점·넣는시점 사이 owner 변경 시 옛 owner 대기열 미등장 | §3 "넣을지 판단…" 내부 확인 조건 |
| 4 | 한 owner 처리권한 동시 하나 | `02` 소유(R76) |
| 5 | 반납 후 재획득 시 늦은 완료 안 섞임 | `02` 소유(R76) |
| 6 | 시간예산 소진 시 다른 owner 진행 | `02` 소유(R77) |
| 7 | 짧은 작업 반복돼도 무한정 점유 안 함 | `02` 소유(R77) |
| 8 | 깨어난 뒤 준비 집합 재확인 | §3 "준비된 owner 집합" 내부 확인 조건 |
| 9 | 한 번 깨어날 때 소켓에서 여러 건 읽고 한도 걸리면 이어 읽음 | §10 "수신 한도가 건수·byte·경과 시간 셋 중 먼저 닿는 것으로 끊기고…" |
| 10 | 한 연결이 계속 보내도 다른 연결 수신 진행 | §10 "한 연결이 계속 보내는 동안에도 다른 연결의 수신이 진행된다" |
| 11 | 수신 한도가 건수·byte·경과 시간 중 먼저 닿는 것으로 끊김 | §10(9와 같은 행에 통합) |
| 12 | 다음 수신 회전이 이번에 멈춘 연결 다음부터 시작 | §10(9와 같은 행에 통합) |
| 13 | 한 socket이 여러 peer 대표 시 peer 단위 회계 | §10 "한 socket이 여러 peer를 대표할 때 회계가 peer 단위로 이루어진다" |
| 14 | 수신 콜백 안에서 handler 미실행 | §5 내부 확인 조건(R83과 같은 문단) |
| 15 | 형식 안 맞는 입력이 handler에 미도달 | §10 "형식이 맞지 않는 입력은 handler에 도달하지 않는다…" |
| 16 | timer 자원 수가 등록 수에 비례해 안 늘어남 | `04-spot-timer.ko.md` 소유 |
| 17 | 기본 option에서 밀린 tick이 하나로 합쳐짐 | `04-spot-timer.ko.md` 소유 |
| 18 | catchup option은 정한 개수까지 | `04-spot-timer.ko.md` 소유 |
| 19 | 오래 도는 timer가 tick 통계로 메모리를 늘리지 않음 | `04-spot-timer.ko.md` 소유 |

### 42 §8 "확인할 결과" 14항

| # | 옛 항목(요약) | 새 위치 |
|---|---|---|
| 1 | Application handler 대기 중 그 호출 timeout 발동 | `02` 소유 |
| 2 | Application handler 대기 중 종료 절차 진행 | `02` 소유 |
| 3 | Application handler 대기 중 새 peer 연결 수락 | `02` 소유 |
| 4 | 느린 상태 구독자가 처리 속도를 안 늦춤 | `02` 소유 |
| 5 | Shared permit 모두 예약되면 cancellable wait, terminal completion 계속 | §10 "Shared permit이 모두 예약되면 ordinary ingress가 cancellable wait하고…" |
| 6 | Core receive byte HWM 찼을 때 backpressure 전달, record 안 버림 | §10 "Core receive byte HWM이 찼을 때 sender까지 backpressure가 전달되며…" |
| 7 | Owner structural reject와 shared-cap wait가 다른 error/metric | §10 "Owner structural reject와 shared-cap wait가 서로 다른 error·metric으로 관찰된다" |
| 8 | 이미 완료된 호출 뒤 실패는 caller 결과 불변, 관측에만 남음 | §10 "이미 완료된 호출 뒤의 실패(…)는 caller 결과를 바꾸지 않고 관측에만 남는다" |
| 9 | application 문맥에서 infra 전용 작업 호출 시 대기 없이 실패 | `02` 소유 |
| 10 | infra 실행 자원이 topology·Spot 수에 비례해 안 늘어남 | `02` 소유 |
| 11 | owner마다 application·lifecycle FIFO 분리 | `02` 소유(S9, 41 소유) |
| 12 | 비어 있던 FIFO에 첫 작업 들어오면 즉시 깨움 | `02` 소유 |
| 13 | 송신 공간 기다리는 작업이 실행 권한을 안 쥠 | §10 "송신 공간을 기다리는 작업이 실행 권한을 쥐고 있지 않다" |
| 14 | 송신 대기 자리가 가득 차면 대기 없이 실패 | §10 "송신 대기 자리가 가득 차면 기다리지 않고 `DeadlineExceeded`로 끝낸다" |

## 이동 후 갱신할 링크

이 문서가 사용하는 옛 경로(아직 이동하지 않은 문서를 가리키는 상대 링크). 캠페인 §5의 en 작성
+ 이동 단계에서 새 경로로 일괄 치환해야 한다.

| 링크 | 현재 형태 | 비고 |
|---|---|---|
| 용어집 | `../01-glossary.ko.md#core-hwm-budget`, `#application-job-queue`, `#routemesh`, `#deadlineexceeded`, `#backpressured` | 용어집이 `00-foundation`으로 이동하면 경로 갱신 |
| Framework API | `../06-framework-api.ko.md` | 아직 주제 미배정 — topic-map 확인 필요 |
| Runtime 상태 | `../24-runtime-monitoring.ko.md` | 아직 주제 미배정 |
| RouteMesh topology | `../07-channel-topology.ko.md` | `02-channel-transport` 주제로 이동 예정(S10) |
| MeshNode startup | `../13-mesh-node.ko.md` | `02-channel-transport` 주제로 이동 예정(S10) |
| Runtime metric | `../06-observability/02-runtime-metrics.ko.md` | README §3.6.1·과제 지시대로 이미 새 경로 사용 — 06-observability 작성 완료 뒤 실재 확인 필요 |

같은 주제 안의 형제 문서 링크(`README.ko.md`, `02-handler-turn-and-execution-gate.ko.md`,
`04-spot-timer.ko.md`, `06-payload-ownership-and-codec.ko.md`)는 이미 최종 상대 경로이며 이동
시 갱신 대상이 아니다. `04-session/01-stream-session.ko.md`·`04-session/02-session-actor-binding.ko.md`
링크도 최종 경로다 — 다만 두 문서 쪽 이관 pointer(각 문서 §9·§10 말미)는 아직 옛 경로
(`../46-internal-dispatch-loop.ko.md`)를 가리키므로, 이 문서가 완료된 뒤
`01-execution/04-application-job-queue-and-backpressure.ko.md#3-ordinary-ingress-permit-순서`로
갱신해야 한다(mapping §3.6.1, §6 — 이 작업은 이 에이전트의 산출물 범위 밖이다).

## spec-gap 후보

새로 발견한 것 없음. `mapping.ko.md`의 기존 G1·G2·G3(§43 dispatcher 4,096 상한 범위, 46§4·41§2
owner 점유 예산 관계, 42§7·41§2 FIFO 우선순위 소유)은 모두 이 문서의 범위 밖(01·02 유래)이므로
여기서 다루지 않는다.
