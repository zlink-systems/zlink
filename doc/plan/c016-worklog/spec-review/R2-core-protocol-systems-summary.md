# R2 Core protocol·systems 심층 검토

지정된 protocol·systems 한국어/영어 28개 파일, 4,550줄을 전부 읽고 Core C++ 구현의 관련 호출 경로를 정적으로 대조했다. 구현·spec·test는 변경하지 않았으며 build, test, benchmark도 실행하지 않았다. 아래 성능 영향은 호출 경로에서 확인한 비용이며 측정 결과가 아니다.

`행동 변경: 없음`은 제안대로 문서의 중복·오래된 설명만 정리했을 때 현재 공개 동작과 주 계약이 유지된다는 뜻이다. 런타임 변경이 필요한 F-R2-5·6은 `있음`으로 분리했다. 규칙 수의 산정 단위는 각 항목에 적었다. 한·영 번역본은 두 규칙으로 세지 않으며, 같은 규칙의 별도 소유 위치는 중복 정의로 센다. 구체적인 입력 조합을 비교하는 F-R2-3·11은 판정 분기를 센다.

## 요약 표

README의 우선순위에 따라 행동 변경 없는 scattered-control, consolidation, 구현 불일치·parity, form 순으로 배치했다. 행동 변경이 있는 lower-layer-reverification은 즉시 적용 후보와 분리하여 구현 변경 항목의 첫머리에 놓았다.

| 번호 | 제목 | 분류 | 행동 변경 | 규칙 수 | 성능 영향 | 확신 |
|---|---|---|---|---|---|---|
| F-R2-1 | correlation 거절의 WRITABLE 조건 소유가 둘이다 | scattered-control | 없음 | 2 → 1 | 없음 | 높음 |
| F-R2-2 | frame 회계 계약을 세 문서가 다시 정의한다 | consolidation | 없음 | 3 → 1 | 없음 | 높음 |
| F-R2-3 | inproc HWM의 대칭 조합 9개는 집합 판정 3개다 | consolidation | 없음 | 9 → 3 | 없음 | 높음 |
| F-R2-4 | thread 계약 요약에 별도 bounded-close 선택지가 남았다 | consolidation | 없음 | 3 → 1 | 없음 | 높음 |
| F-R2-5 | 선택된 pipe를 LB와 write가 연속 admission 검사한다 | lower-layer-reverification | 있음 | 2 → 1 | 있음·정적 비용 | 높음/순서 영향 중간 |
| F-R2-6 | reciprocal 연결이 REJECT 검사를 우회한다 | spec-impl-drift | 있음 | 3 → 2 | 없음 | 높음 |
| F-R2-7 | ZMP 내부 설명에 사라진 pre-admission payload pool이 남았다 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R2-8 | RAW의 빈 payload 금지가 유효한 빈 packet까지 포함한다 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R2-9 | READY 검증 요구의 metadata 기본값 문장이 예외를 잃었다 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R2-10 | 비 DEALER·ROUTER의 lane property 거부가 영어에서 빠졌다 | parity-gap | 없음 | 2 → 1 | 없음 | 높음 |
| F-R2-11 | 일반 connection의 least-load 설명이 현재 선택 경로와 다르다 | spec-impl-drift | 없음 | 3 → 2 | 없음 | 높음 |
| F-R2-12 | HWM 축소 시 admission과 applied snapshot을 같은 적용으로 부른다 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R2-13 | hot-path 문서가 retryable errno만으로 token 발급을 정의한다 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R2-14 | I/O thread 검증 절이 내부 배치·이름을 계약으로 다시 만든다 | form | 없음 | 2 → 1 | 없음 | 높음 |

## Findings

### F-R2-1 correlation 거절의 WRITABLE 조건 소유가 둘이다

- 분류: scattered-control
- 위치: `core/doc/spec/core/systems/06-auto-hwm.ko.md:490–519`, `:577–580`; 영어 `core/doc/spec/core/systems/06-auto-hwm.en.md:379–408`, `:456–459`. 같은 API 결과·wake 규칙은 `core/doc/spec/core/socket/README.ko.md:1060–1077`, 영어 `core/doc/spec/core/socket/README.en.md:1155–1181`에도 있다. 최신 결정은 `doc/plan/c016-worklog/decisions.ko.md:1277–1279`(D-B119), `:1294–1297`(D-B120)이다.
- 현재 규칙(인용): “그 토큰은 **해당 pair의 correlation reservation이 반환될 때 … 만** WRITABLE을 발행” / “physical write credit 회복만으로는 … 발행하지 않는다”.
- 문제: AutoHWM의 `Internals` 아래 work-budget 절이 수용량뿐 아니라 공개 submit 결과와 completion 통지 조건까지 다시 소유한다. 구현에는 거절 자원별 wake 구분이 실제로 있으며 이는 없앨 상태가 아니다. `BACKPRESSURED`는 반환 결과, `EAGAIN`은 errno, token은 거절된 제출의 식별자, `WRITABLE`은 후속 completion이므로 호출자가 구별한다. 줄일 대상은 이들 API 개념이 아니라 공개 wake 규칙의 소유 위치다.
- 제안: **Socket 공통 REQUEST DONTWAIT 절**에 “대기 토큰은 자신의 제출을 거절한 자원이 회복될 때 한 번 WRITABLE을 발행하며, correlation work·count 거절의 회복은 해당 pair의 reservation 반환이고 token의 terminal 조건은 공통 completion 계약을 따른다.”를 합친 규칙으로 두고 AutoHWM은 work·count 계산·반환 메커니즘과 그 계약 참조를 소유한다.
- 규칙 수: before 2 → after 1 — 공개 wake 판정을 독립적으로 정의하는 Socket 공통·AutoHWM 두 위치를 하나로 합친다.
- 행동 변경: 없음 — D-B119/120의 거절 원인 보존, reservation-return wake와 token terminal 조건을 유지하는 문서 정리다.
- 영향: core — `core/src/api/socket/socket_completion_queue_internal.cpp:380–419`; 다른 언어의 binding/runtime은 검증하지 않았다.
- 성능 영향: 없음 — epoch, token 등록 시 재확인, 물리 credit wake를 삭제하지 않는다.
- 근거 코드: C++ `core/src/runtime/core/pipe.cpp:1565–1617`은 work/count 거절과 release epoch를 기록한다; `core/src/runtime/core/pipe.cpp:1625–1658`은 reservation 반환에서 owner activation을 예약한다; `core/src/runtime/sockets/common/socket_send_complete.cpp:199–228`은 correlation 대기를 구분해 등록 후 재확인한다; `core/src/api/socket/socket_completion_queue_internal.cpp:380–419`은 물리 credit만으로 correlation token을 꺼내지 않고 한 번 ready queue로 이동시킨다.
- 확신: 높음 — D-B119/120, 두 언어의 문장, token 등록·게시 경로가 일치한다.

### F-R2-2 frame 회계 계약을 세 문서가 다시 정의한다

- 분류: consolidation
- 위치: `core/doc/spec/core/systems/05-connection-memory.ko.md:51–63`, `:130–137`; 영어 `core/doc/spec/core/systems/05-connection-memory.en.md:52–65`, `:136–145`. 주 정의·메커니즘·관찰 요구는 `core/doc/spec/core/systems/06-auto-hwm.ko.md:304–309`, `:384–401`, `:417–448`, `:469–478`, `:574–576`, `:582–584`; 영어 `core/doc/spec/core/systems/06-auto-hwm.en.md:247–252`, `:305–315`, `:329–353`, `:364–368`, `:453–455`, `:461–463`. Socket 공통에도 `core/doc/spec/core/socket/README.ko.md:444–448`, 영어 `core/doc/spec/core/socket/README.en.md:469–474`로 charge 산식·반환을 다시 정의한다.
- 현재 규칙(인용): “payload와 `sizeof(zlink_msg_t)`를 byte charge로 계산” / “마지막 frame … counter를 다시 증가시키지 않는다” / “complete message를 dequeue … queue charge가 끝나고 writer credit을 반환”.
- 문제: Connection Memory는 AutoHWM 결과의 요약이라고 밝히지만 별도 검증 절까지 같은 counter 전이와 dequeue 경계를 반복한다. Socket 공통에도 산식이 있다. 한 변경에 세 계약 위치를 맞춰야 하며, 연결당 비용 설명에 필요한 allocator 비용과 queue charge의 구별보다 회계 절차가 더 커졌다. Decoder 사전 credit 획득 메커니즘과 앱이 조회하는 snapshot 결과도 같은 목록에서 섞인다.
- 제안: **AutoHWM의 byte 회계 계약·§5 검증 요구**에 “Core는 application frame의 payload와 고정 message 비용을 그 frame이 속한 queue에 한 번 계상하고 multipart 완료에서는 중복 계상 없이 provisional을 committed로 바꾸며 실제 queue 제거에서 charge를 반환하므로 앱의 후속 payload 보유 수명은 이 회계를 연장하지 않는다.”를 합친 규칙으로 두고 Socket 공통·Connection Memory는 이를 참조한다.
- 규칙 수: before 3 → after 1 — 산식·수명 경계를 독립적으로 정의하는 문서 소유자 수다; AutoHWM 내부의 메커니즘 설명과 공개 snapshot 검증은 서로 다른 층으로 남긴다.
- 행동 변경: 없음 — frame 비용, multipart 전이, dequeue 경계와 credit batching을 모두 유지한다.
- 영향: core — `core/src/runtime/core/pipe.cpp:3591–3605`; 언어별 payload 보유 정책은 검증하지 않았다.
- 성능 영향: 없음 — accounting·credit 코드를 바꾸는 제안이 아니다.
- 근거 코드: C++ `core/src/runtime/core/pipe.cpp:1253–1367`은 decoder credit을 payload 할당 이전 경계에서 판정한다; `core/src/runtime/core/pipe.cpp:2460–2580`은 write의 multipart charge·commit을 처리한다; `core/src/runtime/core/pipe.cpp:3591–3605`는 고정 message 비용을 더한다; `core/src/runtime/core/pipe.cpp:3905–3967`은 read-credit 게시와 대기 writer의 wake를 처리한다.
- 확신: 높음 — 동일한 산식·수명 규칙의 세 소유 위치와 pipe 구현을 확인했다.

### F-R2-3 inproc HWM의 대칭 조합 9개는 집합 판정 3개다

- 분류: consolidation
- 위치: `core/doc/spec/core/systems/06-auto-hwm.ko.md:125–142`, 영어 `core/doc/spec/core/systems/06-auto-hwm.en.md:97–112`; 중복 요약 `core/doc/spec/core/socket/README.ko.md:1244–1247`, 영어 `core/doc/spec/core/socket/README.en.md:1380–1384`.
- 현재 규칙(인용): `Finite manual | Auto`, `Auto | Finite manual`, `Unlimited manual | Finite manual`, `Finite manual | Unlimited manual`이 모두 “Finite manual cap”이다.
- 문제: 송·수신 endpoint의 위치를 구분하지 않는 cap 계산을 순서가 있는 9개 경우로 열거했다. 구현은 이미 endpoint를 훑으며 `finite_manual_seen`, `finite_manual_hwm`, `auto_seen`을 집계한 뒤 결정한다. 대칭 사례마다 별도 규칙을 기억할 이유가 없고, Socket 공통의 별도 서술도 유지할 필요가 없다.
- 제안: **AutoHWM의 inproc physical-queue cap 절**에 “동일 physical queue의 endpoint 중 유한 manual 값이 있으면 그 최솟값을 cap으로, 없고 auto가 있으면 water-filling 값을 cap으로, 모두 unlimited이면 admission 무제한과 역할별 상한 한 번의 계획 reservation을 적용한다.”를 합친 규칙으로 둔다.
- 규칙 수: before 9 → after 3 — table의 순서 있는 조합 9개를 유한값 존재·auto 존재·전부 unlimited의 배타적 판정 3개로 줄이고 Socket 공통의 재정의는 참조로 바꾼다.
- 행동 변경: 없음 — 9개 입력 조합의 결과, physical queue를 한 번 세는 기준과 unlimited의 계획 reservation을 보존한다.
- 영향: core — `core/src/runtime/core/ctx_physical_queue_registry.cpp:843–876`; binding별 HWM option wrapper는 검증하지 않았다.
- 성능 영향: 없음 — 구현은 이미 이 집합 판정을 사용한다.
- 근거 코드: C++ `core/src/runtime/core/ctx_physical_queue_registry.cpp:843–851`은 유한값 최솟값과 auto 존재를 집계한다; 같은 파일 `:865–876`은 이를 하나의 queue plan으로 만든다; `core/src/runtime/core/auto_hwm_policy.cpp:387–408`은 auto와 unlimited manual의 reservation을 구별한다.
- 확신: 높음 — 표의 모든 입력 조합을 세 판정에 대응시킬 수 있다.

### F-R2-4 thread 계약 요약에 별도 bounded-close 선택지가 남았다

- 분류: consolidation
- 위치: 계약 소유 선언 `core/doc/spec/core/systems/04-thread-safety.ko.md:22–28`, 영어 `core/doc/spec/core/systems/04-thread-safety.en.md:23–29`; 재서술 `:35–39`, `:53–61` / 영어 `:37–41`, `:55–64`; 또 다른 요약 `core/doc/spec/core/systems/02-threading-model.ko.md:59–68`, 영어 `core/doc/spec/core/systems/02-threading-model.en.md:59–69`. 주 계약 `core/doc/spec/core/socket/README.ko.md:44–58`, 영어 `core/doc/spec/core/socket/README.en.md:48–63`.
- 현재 규칙(인용): “active callback이나 API가 있으면 bounded close 계약에 따라 기다리거나 `BUSY`”와 “다른 thread가 … admitted API를 실행 중이면 `EBUSY`”가 함께 존재한다.
- 문제: 두 systems 문서는 caller 계약을 소유하지 않는다고 하면서 허용 동시성·close 결과를 다시 말한다. 그 요약에는 주 계약에 없는 ‘기다리거나’ 선택지가 남아 있다. guard가 admission 수와 closing bit를 관리한다는 내부 설명과 호출자가 관찰하는 결과를 분리하면 이 선택지를 둘 필요가 없다.
- 제안: **Socket 공통 §2**의 계약만 남기고 systems 문서에는 “공개 핸들의 동시 호출 허용 범위와 close 결과는 Socket 공통 §2가 소유하며 lifecycle coordinator는 admitted API 수와 closing state로 그 계약을 집행한다.”를 합친 참조 문장으로 둔다.
- 규칙 수: before 3 → after 1 — Socket 공통·Threading Model·Thread Safety의 caller 계약 정의를 한 소유자로 합친다.
- 행동 변경: 없음 — 현재 fail-fast `EBUSY`와 accepted-close 이후 `ESHUTDOWN`을 유지하며, 내부 reaper 수명과 공개 close 반환을 혼동한 문장만 정리한다.
- 영향: core — `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp:277–301`; service·binding의 별도 close 계약은 검증하지 않았다.
- 성능 영향: 없음 — guard, lock, reaper 구현 변경이 없다.
- 근거 코드: C++ `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp:58–85`는 closing 이후 API를 `ESHUTDOWN`으로 거부한다; 같은 파일 `:277–301`은 inflight API가 있으면 기다리지 않고 `EBUSY`를 반환한다; 같은 파일 `:18–28`은 lifecycle state word의 소유 필드를 설명한다.
- 확신: 높음 — 공개 close gate에서 대기 선택지가 없음을 확인했다.

### F-R2-5 선택된 pipe를 LB와 write가 연속 admission 검사한다

- 분류: lower-layer-reverification
- 위치: hot-path 호출 트리 `core/doc/spec/core/systems/10-hot-path.ko.md:28–43`, 영어 `core/doc/spec/core/systems/10-hot-path.en.md:31–47`; 비용·캐시 규칙 한국어 `:45–95`, 영어 `:49–103`. Admission 책임은 `core/doc/spec/core/systems/06-auto-hwm.ko.md:407–448`, 영어 `core/doc/spec/core/systems/06-auto-hwm.en.md:319–353`, ownership commit 설명은 `core/doc/spec/core/systems/04-thread-safety.ko.md:45–49`, 영어 `core/doc/spec/core/systems/04-thread-safety.en.md:47–52`에 있다.
- 현재 규칙(인용): 호출 트리는 “`lb_t::sendpipe_to` → `pipe_t::write_*`”이고, admission은 “message ownership 전이와 함께 commit”한다.
- 문제: `sendpipe_to()`는 선택된 pipe에 `check_write_admission()`을 호출하고 이어서 같은 pipe의 write를 호출한다. 전자는 `_out_sync`를 잡고 state/HWM을 검사하며, 후자는 다시 lock을 잡고 state와 실제 candidate charge를 판정한다. 사전 검사의 성공은 lock을 놓은 뒤 write의 성공을 보장하지 못한다. 일반 LB의 직접 write 경로와 distributor도 검토했으며, 모든 peer의 admission을 먼저 확인해야 하는 PUB fan-out의 `check_hwm()`은 이 원인에 포함하지 않았다.
- 제안: **AutoHWM의 message-path admission 책임 절**, Hot Path는 참조: “선택된 pipe의 write가 대상 message의 physical admission과 ownership commit을 소유하고 LB는 peer 선택과 그 write 결과에 따른 활성 집합 갱신만 수행한다.”
- 규칙 수: before 2 → after 1 — 동일 physical admission을 LB의 사전 검사와 pipe write 두 단계가 결정하는 구조를 pipe 한 소유자로 합친다.
- 행동 변경: 있음 — 사전 검사 삭제만으로는 무행동 변경을 보장할 수 없다; observed REQUEST는 write 내부의 correlation observer가 physical admission보다 먼저 실행되므로 physical HWM과 work/count가 동시에 부족할 때 기록되는 거절 원인·후속 WRITABLE 조건이 달라질 수 있고, 과대 message와 이미 찬 HWM의 오류 우선순위도 검토해야 한다. 0.18.0 검토 대상이다.
- 영향: core — `core/src/runtime/sockets/internal/lb.cpp:302–342`; 다른 언어 runtime은 검증하지 않았다.
- 성능 영향: 있음 — 해당 선택 경로의 성공 제출마다 사전 `_out_sync` acquire/release와 state/HWM 읽기 한 묶음을 제거할 여지가 있다; 개선량은 측정하지 않았다.
- 근거 코드: C++ `core/src/runtime/sockets/dealer/dealer.cpp:289–312`는 선택된 pipe를 이 경로로 보낸다; `core/src/runtime/sockets/internal/lb.cpp:302–342`는 사전 검사와 write 결과 처리를 반복한다; `core/src/runtime/core/pipe.cpp:1809–1844`는 사전 검사의 lock·판정을 보인다; 같은 파일 `:2583–2663`은 실제 write가 candidate 크기를 포함해 다시 판정한다; 같은 파일 `:2666–2708`은 REQUEST observer와 physical admission 순서가 달라질 수 있음을 보인다.
- 확신: 높음 — 중복 lock·판정은 직접 확인했다; 중간 — 경쟁 조건별 공개 결과의 변화 범위는 실행 없이 확정하지 않았다.

### F-R2-6 reciprocal 연결이 REJECT 검사를 우회한다

- 분류: spec-impl-drift
- 위치: RID 정책의 소유자는 `core/doc/spec/core/socket/README.ko.md:149–173`, 영어 `core/doc/spec/core/socket/README.en.md:154–186`; terminal 규칙은 한국어 `:1157`, 영어 `:1277`. Lane-set 유효성의 별도 소유자는 `core/doc/spec/core/protocol/01-zmp.ko.md:186–211`, 영어 `core/doc/spec/core/protocol/01-zmp.en.md:192–218`이며 여기에 RID admission의 세 번째 정책은 없다. 결정 `doc/plan/c016-worklog/decisions.ko.md:1123–1127`(D-B96), `:1281–1285`(D-094).
- 현재 규칙(인용): “`ZLINK_RID_DUPLICATE_REJECT`는 기존 pipe를 유지하고 새 중복 pipe를 등록하지 않으며 … 즉시 닫는다”; D-094는 “REJECT/HANDOVER; reciprocal collapse D-B96은 HANDOVER의 하위 규칙”이라고 정한다.
- 문제: `_handover == false`여도 새 pipe가 paired Application이고 기존 route와 반대 방향이면 REJECT 분기를 건너뛴다. 뒤의 RID 비교 결과에 따라 기존 route를 교체하거나 새 route를 standby로 등록한다. 따라서 ‘REJECT인데 reciprocal이면 HANDOVER 경로’라는 세 번째 결정이 구현에 남았다. 이는 wire lane-count 검증과 다른 socket routing 정책이며 ZMP에 예외로 적어 맞출 사안이 아니다.
- 제안: **Socket 공통 §4 RID 정책**의 단일 판정 문장으로 “기존 RID가 있으면 REJECT는 새 중복 pair를 닫고 HANDOVER만 같은 방향 교체 또는 reciprocal RID 비교에 의한 방향 선택을 수행한다.”를 두고 구현의 reciprocal REJECT 우회를 제거하는 방향으로 검토한다.
- 규칙 수: before 3 → after 2 — REJECT·HANDOVER·REJECT reciprocal 우회에서 두 정책만 남긴다; HANDOVER 안의 방향 선택·standby 규칙은 새 정책으로 세지 않는다.
- 행동 변경: 있음 — 기본 REJECT에서도 살아남던 반대 방향 pair가 닫히며 monitor, route 선택, 그 pair의 admitted request terminal 결과가 달라질 수 있다; 0.18.0 대상이다.
- 영향: core — `core/src/runtime/sockets/router/router_admission.cpp:343–374`; bindings의 enum 전달은 검증하지 않았다.
- 성능 영향: 없음 — 이 항목의 목적은 admission 정책 정합성이며 메시지별 비용 개선을 주장하지 않는다.
- 근거 코드: C++ `core/src/runtime/sockets/router/router.cpp:35`, `:81–91`은 `_handover`를 공개 정책에서 직접 설정한다; `core/src/runtime/sockets/router/router_admission.cpp:343–374`은 reciprocal 예외와 standby 등록을 보인다; 같은 파일 `:303–335`은 그 후의 방향 선택을 수행한다; 같은 파일 `:393–466`은 route 교체와 superseded pair의 pending request 종결을 연결한다.
- 확신: 높음 — REJECT 옵션 값에서 우회 조건까지 직접 이어진다. 공개 API 실행 재현은 이 read-only job에서 하지 않았다.

### F-R2-7 ZMP 내부 설명에 사라진 pre-admission payload pool이 남았다

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/protocol/01-zmp.ko.md:469–475`, 영어 `core/doc/spec/core/protocol/01-zmp.en.md:499–506`; 현재 option 계약 `core/doc/spec/core/socket/README.ko.md:388–403`, 영어 `core/doc/spec/core/socket/README.en.md:405–421`; REQUEST payload-free 계약 한국어 `:1060–1077`, 영어 `:1155–1181`.
- 현재 규칙(인용): “DONTWAIT request가 admission 전에 payload를 보관하면 `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`의 SEND·REQUEST 공유 pool도 사용한다.”
- 문제: Socket 공통은 두 option이 ABI 저장·조회 전용이며 admission 전 SEND·REQUEST payload 보관 상태가 없다고 명시한다. Core 전체에서 두 저장 필드의 참조는 초기값·선언·set/get에 한정된다. ZMP의 조건부 문장은 현재 도달할 수 없는 과거 구현 상태를 가능한 내부 경로로 남긴다. Completion slot과 payload pool을 하나의 ‘pending’이라고 다루면 현재 65,536 shared slot의 실제 역할도 흐려진다.
- 제안: **Socket 공통의 pending option·submit 계약**을 소유자로 하고 ZMP §9에는 “Core의 admission 전 보관 상태는 Socket 공통의 payload-free completion reservation 계약을 따르며 `PENDING_MAX_MSGS/BYTES`는 ABI 저장·조회 외 동작에 영향을 주지 않는다.”를 합친 참조 문장으로 둔다.
- 규칙 수: before 2 → after 1 — ZMP의 payload-pool 모델과 Socket 공통의 payload-free 모델 중 현재 모델만 남긴다.
- 행동 변경: 없음 — 삭제 대상은 실행되지 않는 설명이며 option ABI, completion slot, timeout 시작 시점은 그대로다.
- 영향: core — `core/src/runtime/core/options_core_socket.cpp:121–135`; binding별 option wrapper는 검증하지 않았다.
- 성능 영향: 없음 — 현재 없는 payload FIFO를 새로 만들거나 기존 completion reservation을 줄이지 않는다.
- 근거 코드: C++ `core/src/runtime/core/options_core_socket.cpp:121–135`는 값을 저장하고 같은 파일 `:292–305`는 반환한다; `core/src/api/socket/socket_request_reply_submit_api.cpp:172–205`는 admission 성공 뒤 timeout을 시작한다; 같은 파일 `:249–273`은 실패 상태를 정리하고 대기 토큰을 처리한다; `core/src/api/socket/socket_completion_queue_internal.cpp:195–296`은 completion reservation과 wait record를 소유한다.
- 확신: 높음 — 저장 필드의 전체 참조 검색과 submit 경로가 현재 계약을 지지한다.

### F-R2-8 RAW의 빈 payload 금지가 유효한 빈 packet까지 포함한다

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/protocol/02-raw.ko.md:63–68`, `:106–110`; 영어 `core/doc/spec/core/protocol/02-raw.en.md:64–69`, `:109–114`. API 소유 선언은 한국어 `:13–15`, 영어 `:13–15`. 반대되는 구체적 API 계약은 `core/doc/spec/core/socket/08-stream.ko.md:196–197`, `:210–211`, `:460`; 영어 `core/doc/spec/core/socket/08-stream.en.md:207–208`, `:220–222`, `:496–497`이다.
- 현재 규칙(인용): RAW는 “raw/packet 경로의 0 byte payload는 … application 데이터로 전달되지 않는다”고 하지만 STREAM은 “`header_size == 0 && body_size == 0`”을 두 개의 유효한 빈 message로 반환한다.
- 문제: transport의 빈 연결 알림과 6-byte prefix가 있는 application packet의 빈 header/body를 ‘0 byte payload’로 묶었다. 완전한 `0 + 0` packet은 길이 필드가 있는 application 데이터이며 decoder는 두 빈 message를 queue에 넣는다. RAW에서 앱의 packet 결과까지 다시 규정하면서 계약이 충돌했다.
- 제안: **STREAM packet receive 계약**을 API 소유자로 두고 RAW §4에는 “연결 알림을 나타내는 빈 transport 입력은 monitor 이벤트로 처리하며 PACKET의 application 결과는 STREAM 계약을 따라 완전한 6-byte prefix의 `0 + 0` packet도 유효하다.”를 합친 경계 문장으로 둔다.
- 규칙 수: before 2 → after 1 — packet의 빈 application 데이터 수용 여부를 결정하는 API 규칙을 STREAM 한 곳에 둔다.
- 행동 변경: 없음 — 현재 parser와 이미 명시된 STREAM 공개 계약을 유지하면서 RAW 문장의 ‘빈 값’ 범위를 바로잡는다.
- 영향: core — `core/src/runtime/sockets/stream/stream.cpp:538–585`; 다른 언어 packet API는 검증하지 않았다.
- 성능 영향: 없음 — packet parser·queue·drain 구현 변경이 없다.
- 근거 코드: C++ `core/src/runtime/sockets/stream/stream.cpp:538–585`는 header/body 길이 0을 허용하고 두 part를 enqueue한다; 같은 파일 `:417–442`는 두 message를 packet record로 옮긴다; 같은 파일 `:715–805`는 raw chunk를 packet 조립 입력으로 처리하며 packet 출력 여부를 구분한다.
- 확신: 높음 — 6-byte prefix와 0-byte transport payload의 구별을 코드와 STREAM 한·영 계약에서 확인했다.

### F-R2-9 READY 검증 요구의 metadata 기본값 문장이 예외를 잃었다

- 분류: spec-impl-drift
- 위치: 정의 `core/doc/spec/core/protocol/01-zmp.ko.md:163–168`, `:186–190`; 영어 `core/doc/spec/core/protocol/01-zmp.en.md:167–173`, `:192–196`. 모순되는 검증 문장 한국어 `:506–508`, 영어 `:553–557`.
- 현재 규칙(인용): “기본값(비활성)이면 metadata property가 없고” 다음 항목은 “DEALER·ROUTER … option과 관계없이 … metadata가 항상 있다”고 한다.
- 문제: §4 본문은 DEALER·ROUTER의 강제 metadata를 정확히 설명하지만 §10의 기본값 검증 항목은 socket-type 조건을 생략했다. 이 항목 하나만 test로 옮기면 정상적인 DEALER·ROUTER READY를 잘못 거부한다. 강제 metadata에는 `Zlink-Max-Message-Size`도 들어가므로 필수 lane property 목록만으로 전체 READY를 설명해서도 안 된다.
- 제안: **ZMP §10의 READY metadata 관찰 계약**에 “READY는 DEALER·ROUTER이거나 `ZLINK_OPT_ZMP_METADATA`가 켜진 경우 basic metadata를 싣고 DEALER·ROUTER이면 RID와 lane metadata를 추가하며, 그 밖의 경우에만 metadata가 없다.”를 합친 판정으로 두고 §4는 metadata의 byte 배치와 이 계약의 참조를 남긴다.
- 규칙 수: before 2 → after 1 — ‘기본값이면 무조건 없음’과 별도 paired 예외를 하나의 metadata 활성 조건으로 합친다.
- 행동 변경: 없음 — 기본 option 값·실제 READY byte·기존 §4 계약을 보존한다.
- 영향: core — `core/src/runtime/engine/asio/asio_zmp_engine.cpp:680–688`; 다른 언어의 독립 ZMP encoder는 검증하지 않았다.
- 성능 영향: 없음 — handshake 코드 변경이 없다.
- 근거 코드: C++ `core/src/runtime/protocol/zmp_control.hpp:102–116`은 metadata가 켜졌을 때 basic properties를 만든다; `core/src/runtime/engine/asio/asio_zmp_engine.cpp:680–688`은 paired READY에서 metadata를 강제한다; `core/src/runtime/protocol/zmp_metadata.hpp:73–100`은 maximum·RID·lane property의 포함 조건을 구현한다.
- 확신: 높음 — 기본 encoder와 paired 호출자의 옵션 강제까지 확인했다.

### F-R2-10 비 DEALER·ROUTER의 lane property 거부가 영어에서 빠졌다

- 분류: parity-gap
- 위치: `core/doc/spec/core/protocol/01-zmp.ko.md:198–199`, `:514–515`; 영어 `core/doc/spec/core/protocol/01-zmp.en.md:203–204`, `:566–567`. STREAM이 ZMP를 사용하지 않는 경계는 `core/doc/spec/core/protocol/02-raw.ko.md:31–43`, 영어 `core/doc/spec/core/protocol/02-raw.en.md:32–44`다.
- 현재 규칙(인용): 한국어는 “이 pattern에서 두 property를 받으면 handshake protocol failure다”; 영어 대응 항목은 “send neither `Zlink-Lane-Count` nor `Zlink-Lane`”에서 끝난다.
- 문제: 영어만 따르면 비 paired ZMP peer의 lane property를 무시해도 되는지 알 수 없다. 실제 parser는 둘 중 하나라도 있으면 실패한다. 동시에 한국어 검증 항목은 PAIR·PUB-SUB와 RAW인 STREAM을 묶어 STREAM도 ZMP READY를 받을 수 있는 것처럼 썼다. 공통 원인은 lane property의 적용 대상을 한 predicate로 정의하지 않고 송신·수신·언어별 목록으로 반복한 것이다.
- 제안: **ZMP §10의 Request-reply lane 관찰 계약**에 “ZMP를 사용하는 비 DEALER·ROUTER endpoint의 READY에는 두 lane property 모두 없어야 하며 어느 하나라도 수신되면 handshake protocol failure로 거부한다.”를 한·영 동일 규칙으로 두고 §4.1은 이 계약을 참조하며 RAW STREAM은 READY 검증 대상 목록에서 제외한다.
- 규칙 수: before 2 → after 1 — 송신 금지와 일부 언어의 별도 수신 금지를 하나의 READY 유효성 조건으로 합친다.
- 행동 변경: 없음 — 기존 parser의 거부 결과와 RAW transport 선택을 명문화하는 parity 정리다.
- 영향: core — `core/src/runtime/engine/asio/asio_zmp_engine.cpp:594–604`; 독립 타 언어 protocol 구현은 검증하지 않았다.
- 성능 영향: 없음 — parser 조건을 추가하거나 강하게 만들지 않는다.
- 근거 코드: C++ `core/src/runtime/engine/asio/asio_zmp_engine.cpp:594–604`는 비 paired endpoint에서 어느 property라도 있으면 `EPROTO`를 반환한다; `core/src/runtime/protocol/zmp_metadata.hpp:90–100`은 해당 property를 DEALER·ROUTER에만 만든다; `core/src/runtime/transports/tcp/asio_tcp_listener.cpp:241–246`은 STREAM에 RAW engine을 선택한다.
- 확신: 높음 — 영어의 누락 구간과 현재 수신·송신 구현을 함께 확인했다.

### F-R2-11 일반 connection의 least-load 설명이 현재 선택 경로와 다르다

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/systems/03-io-thread.ko.md:141–153`, 영어 `core/doc/spec/core/systems/03-io-thread.en.md:143–156`; 구현 서술 경계는 한국어 `:63–65`, 영어 `:64–66`이다.
- 현재 규칙(인용): “일반 connection은 … least-load … STREAM connection은 기본적으로 … round-robin”.
- 문제: `choose()`의 least-load 구현은 존재하지만 일반 network connection의 connector·accepted session은 `choose_transport()`를 호출한다. 그 함수는 affinity 후보를 round-robin으로 고른다. STREAM은 별도 counter의 round-robin이 기본이며 환경 변수 `minload`에서만 `choose()`로 간다. 함수 이름만 보고 일반 connection의 정책을 설명한 내부 서술이 현재 호출 경로를 반영하지 못했다.
- 제안: **I/O Thread §3.4의 구현 서술**에 “Transport connection은 affinity가 허용한 후보를 기본 round-robin으로 선택하며 STREAM의 `ZLINK_ASIO_STREAM_SESSION_SCHED=minload` 설정만 해당 STREAM 선택을 least-load로 바꾼다.”를 합친 설명으로 둔다.
- 규칙 수: before 3 → after 2 — 일반 least-load·STREAM 기본 RR·STREAM override 세 설명 분기를 기본 transport RR·STREAM override 두 분기로 줄인다; 일반/STREAM의 counter 공유를 뜻하지 않는다.
- 행동 변경: 없음 — 현재 selector를 바꾸지 않고 명시적으로 구현 서술인 절을 고친다.
- 영향: core — `core/src/runtime/core/ctx_io_thread_registry.cpp:90–128`; 언어별 context wrapper는 검증하지 않았다.
- 성능 영향: 없음 — 배치 정책이나 부하 분산 알고리즘을 바꾸지 않는다.
- 근거 코드: C++ `core/src/runtime/core/session_base.cpp:843–855`는 일반 connector에 transport selector를 쓴다; `core/src/runtime/transports/tcp/asio_tcp_listener.cpp:249–257`은 accepted session에도 같은 selector를 쓴다; `core/src/runtime/core/ctx_runtime_resources.cpp:82–95`는 registry로 전달한다; `core/src/runtime/core/ctx_io_thread_registry.cpp:90–128`은 일반 RR와 STREAM override를 구현한다.
- 확신: 높음 — selector 본체와 connect·accept 양쪽 호출자를 대조했다. IPC·WS·TLS listener도 동일 호출을 검색으로 확인했지만 본문 전체는 읽지 않았다.

### F-R2-12 HWM 축소 시 admission과 applied snapshot을 같은 적용으로 부른다

- 분류: spec-impl-drift
- 위치: 즉시 차단 규칙 `core/doc/spec/core/systems/06-auto-hwm.ko.md:160–166`, 영어 `core/doc/spec/core/systems/06-auto-hwm.en.md:126–132`; 내부 축소 설명 한국어 `:481–483`, 영어 `:370–372`; 검증 요구 한국어 `:587`, 영어 `:466`; Socket 공통의 deferred shrink 요약 `core/doc/spec/core/socket/README.ko.md:1233–1234`, 영어 `core/doc/spec/core/socket/README.en.md:1366–1368`.
- 현재 규칙(인용): 한 절은 “새 목표를 즉시 기록 … 추가 admission을 막음”, 검증 절은 “drain된 뒤에 새 HWM이 admission에 적용”이라고 한다.
- 문제: 구현은 writer의 `_hwm`에 planned target을 즉시 넣는다. 반면 registry의 `applied_hwm`은 현재 보관량이 새 target 이하가 될 때까지 이전 값을 유지할 수 있다. 이 두 값을 모두 ‘HWM 적용’으로 표현하여 검증 절이 옛 HWM으로 계속 admission하는 것처럼 읽힌다. 기존 message 보존과 snapshot의 deferred 값은 필요하며, 둘을 지울 상태로 보아서는 안 된다.
- 제안: **AutoHWM §5의 HWM 변경 관찰 계약**에 “HWM 축소는 새 planned target으로 추가 admission을 즉시 제한하면서 이미 보관한 frame을 유지하고, 보관량이 target 이하가 되면 snapshot의 deferred shrink를 끝내 applied 값을 target으로 맞춘다.”를 합친 규칙으로 둔다.
- 규칙 수: before 2 → after 1 — admission에 즉시 적용한다는 정의와 drain 뒤 적용한다는 정의를 planned/admission과 applied/snapshot의 한 전이 규칙으로 합친다.
- 행동 변경: 없음 — 현재 admission, 보관 frame, planned/applied/deferred 조회 결과를 바꾸지 않는 용어 정정이다.
- 영향: core — `core/src/runtime/core/pipe.cpp:947–966`; binding의 snapshot DTO는 검증하지 않았다.
- 성능 영향: 없음 — wake 기준·회계 counter·HWM 재계산을 바꾸지 않는다.
- 근거 코드: C++ `core/src/runtime/core/pipe.cpp:947–966`은 writer의 planned HWM 즉시 적용과 reader applied 회계를 구별한다; `core/src/runtime/core/ctx_physical_queue_registry.cpp:947–959`는 planned target을 즉시 기록하고 applied 변경을 조건부로 한다; 같은 파일 `:205–215`는 drain 후 deferred 적용을 끝낸다; `core/src/runtime/core/pipe.cpp:2616–2630`은 writer HWM으로 실제 candidate admission을 판단한다.
- 확신: 높음 — 두 수치의 writer·registry 소유와 갱신 시점을 확인했다.

### F-R2-13 hot-path 문서가 retryable errno만으로 token 발급을 정의한다

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/systems/10-hot-path.ko.md:83–90`, 영어 `core/doc/spec/core/systems/10-hot-path.en.md:91–98`; 반대되는 구체적 target 계약은 `core/doc/spec/core/socket/README.ko.md:1038–1042`, `:1076–1077`, 영어 `core/doc/spec/core/socket/README.en.md:1127–1131`, `:1179–1181`.
- 현재 규칙(인용): “재시도 가능한 거절이면 … wait token을 등록하고 `ZLINK_SUBMIT_BACKPRESSURED`”; 앞 문장에 `EHOSTUNREACH`가 포함된다.
- 문제: 같은 `EHOSTUNREACH`라도 존재하지 않는 mandatory ROUTER RID는 `NOT_CONNECTED`, ID 0이며 token을 만들지 않는다. Hot Path가 API의 target 존재 여부를 잃은 errno 기반 대체 판정을 정의했다. 이 문서가 소유해야 하는 것은 token 등록이 성공 경로 밖에 있다는 비용 경계다.
- 제안: **Hot Path §4에는 비용 규칙만** “DONTWAIT의 token 발급 여부는 Socket 공통 submit 계약의 target·admission 판정을 따르며 token 등록은 거절 경로에만 있고 정상 성공 제출 경로에는 없다.”를 합친 문장으로 둔다.
- 규칙 수: before 2 → after 1 — errno로 만든 대체 API 판정과 Socket 공통의 target·resource 판정 중 후자만 계약으로 남긴다.
- 행동 변경: 없음 — unknown RID 결과·ID 0·token 미생성 및 기존 wake 경로를 유지한다.
- 영향: core — `core/src/runtime/sockets/common/socket_send_complete.cpp:177–218`; binding의 errno 변환은 검증하지 않았다.
- 성능 영향: 없음 — 성공 경로에 target 확인이나 token 작업을 새로 추가하지 않는다.
- 근거 코드: C++ `core/src/runtime/sockets/common/socket_send_complete.cpp:177–196`은 retryable errno를 확인한 뒤에도 unknown routed target에서 token 예약 전에 반환한다; 같은 파일 `:199–228`은 token 등록과 거절 자원에 따른 재확인을 수행한다; `core/src/api/socket/socket_request_reply_submit_api.cpp:249–273`은 token 등록 결과와 submit 실패 처리를 연결한다.
- 확신: 높음 — hot-path 문장의 충분조건이 실제 API에서는 충분조건이 아니다.

### F-R2-14 I/O thread 검증 절이 내부 배치·이름을 계약으로 다시 만든다

- 분류: form
- 위치: `core/doc/spec/core/systems/03-io-thread.ko.md:179–194`, 영어 `core/doc/spec/core/systems/03-io-thread.en.md:182–199`; 같은 구현의 본 설명은 한국어 `:43–59`, `:141–153`, 영어 `:44–60`, `:143–156`. 판정 기준 `doc/principal/documentation/spec-writing-guide.ko.md:367–418`(§4.3·§4.4), `:701` 이하 §9.3.
- 현재 규칙(인용): “공개 표면 … 과 OS가 노출하는 process의 thread 목록·이름만으로 관찰”하며 “각 항목은 test 하나로 이어진다”.
- 문제: `IO/N` 이름과 connection별 thread 배치는 Core 공개 API 결과가 아니라 내부 자원 배치 관찰이다. 특히 OS thread 이름만으로 특정 connection을 어느 thread에 할당했는지 판별할 수 없다. 구현 서술인 §2·§3의 내용을 다시 contract-test 의무로 만들면서 ‘이 구현을 바꿀 수 있는가’에 두 답이 생긴다. 내부 설명 자체가 spec 파일에 있는 것은 문제로 세지 않았다.
- 제안: **I/O Thread §2·§3 구현 서술**을 소유자로 두고 검증 절에는 “I/O thread의 생성 시점·이름·connection 배치는 구현 점검 대상으로 §2·§3에서 설명하며 `ZLINK_IO_THREADS` 설정·조회와 오류의 공개 계약 검증은 Context spec이 소유한다.”를 합친 경계 문장으로 둔다.
- 규칙 수: before 2 → after 1 — 동일 내부 배치를 구현 설명과 독립 contract-test 의무로 두던 이중 정의에서 구현 설명 한 곳으로 줄인다.
- 행동 변경: 없음 — thread 생성·이름·배치나 공개 API를 바꾸는 제안이 아니며 내부 점검을 삭제하자는 뜻도 아니다.
- 영향: core — `core/src/runtime/core/io_thread.cpp:26–31`; 플랫폼별 OS thread 표시와 binding별 context API는 검증하지 않았다.
- 성능 영향: 없음 — 실행 thread 수·scheduler·poller 비용을 바꾸지 않는다.
- 근거 코드: C++ `core/src/runtime/core/io_thread.cpp:26–31`은 내부 thread 이름을 만든다; `core/src/runtime/core/ctx_runtime_resources.cpp:35–45`는 runtime 자원을 시작한다; `core/src/runtime/transports/tcp/asio_tcp_listener.cpp:249–257`은 accepted connection의 session thread를 선택한다.
- 확신: 높음 — 공개 옵션 결과와 내부 scheduler 배치의 검증 표면이 다르다.

## 추가 후보·비발견

- 동시 count-2 attempt의 무조건 수렴 문장은 BLOCKERS B1로 남겼다: `core/doc/spec/core/protocol/01-zmp.ko.md:201–206`, 영어 `core/doc/spec/core/protocol/01-zmp.en.md:206–212`; D-096 `doc/plan/c016-worklog/decisions.ko.md:1304–1306`에는 계속 충돌해 해당 test를 제거했다는 기록이 있다.
- Gate 실행 시점의 서로 다른 문장은 BLOCKERS B3으로 남겼다: `core/doc/spec/core/systems/10-hot-path.ko.md:99–100`, `:138`; 영어 `core/doc/spec/core/systems/10-hot-path.en.md:107–108`, `:150–151`.
- D-099의 64-chunk step은 RAW spec에 없다. `core/src/runtime/sockets/stream/stream.cpp:715–726`이 기존 receive-progress와 mailbox를 깨우고 `:1175–1197`의 packet readiness는 완성된 packet을 기준으로 한다. `core/doc/spec/core/socket/08-stream.ko.md:461–462`, 영어 `core/doc/spec/core/socket/08-stream.en.md:498–499`의 공개 관찰과 수치 64를 같은 규칙으로 취급하지 않았다. `7738b8fd41`은 변경 목록만 읽었고 그때의 test·성능 결과를 이 job의 실행 결과로 재사용하지 않았다.
- ZMP lane-set 검증과 RID duplicate admission은 합치지 않는다. 전자는 `core/doc/spec/core/protocol/01-zmp.ko.md:186–211` / 영어 `:192–218`과 `core/src/runtime/engine/asio/asio_zmp_engine.cpp:594–631`, `core/src/runtime/sockets/common/socket_base_api.cpp:343–397`의 topology 유효성이고, 후자는 Socket 공통 §4와 `core/src/runtime/sockets/router/router_admission.cpp:343–374`의 route 선택이다. D-096의 old-version·중복 lane 예는 같은 lane 유효성 규칙의 적용 사례이므로 독립 admission 정책으로 더 세지 않았다.
- Reply의 현재 RID 경로 선택과 submit-time pair의 completion 권한도 구별했다. `core/doc/spec/core/protocol/01-zmp.ko.md:325–328` / 영어 `:339–343` 및 `core/src/api/socket/socket_request_reply_runtime_io.cpp:1380–1406`의 current-route fallback만으로 새 pair가 기존 request를 성공 완료한다고 결론 내릴 수 없다: `core/src/api/socket/socket_request_reply_pending_api.cpp:105–145`는 submit-time pair·generation과 source lane을 검증하고 `core/src/api/socket/socket_request_reply_dispatch.cpp:154–159`은 맞지 않는 reply를 버린다.
- Callgrind ±5% 판정은 문서와 현재 gate가 일치한다: `core/doc/spec/core/systems/10-hot-path.ko.md:104–117`, 영어 `:112–127`; `core/tests/perf/hotpath_gate.py:24–26`, `:219–231`. Gate는 실행하지 않았다.

## 읽은 범위

### 지정 spec 전수

아래 경로의 접두어는 `core/doc/spec/core/`다. 모두 첫 줄부터 마지막 줄까지 읽었으며 KO/EN 의미를 대조했다. 두 README와 systems/08·09도 실제 wildcard 범위에 포함되어 읽었다.

| 파일 | 한국어 읽은 줄 수 | 영어 읽은 줄 수 |
|---|---:|---:|
| `protocol/README` | 21 | 22 |
| `protocol/01-zmp` | 600 | 674 |
| `protocol/02-raw` | 119 | 124 |
| `systems/README` | 32 | 34 |
| `systems/01-architecture` | 78 | 84 |
| `systems/02-threading-model` | 68 | 69 |
| `systems/03-io-thread` | 194 | 199 |
| `systems/04-thread-safety` | 61 | 64 |
| `systems/05-connection-memory` | 155 | 166 |
| `systems/06-auto-hwm` | 608 | 487 |
| `systems/07-core-source-layout` | 72 | 73 |
| `systems/08-posd-module-structure` | 63 | 65 |
| `systems/09-design-decisions` | 63 | 63 |
| `systems/10-hot-path` | 139 | 153 |
| **합계** | **2,273** | **2,277** |

### 기준·교차 참조

- 먼저 읽은 지침: `AGENTS.md` 137줄, `doc/plan/c016-worklog/spec-review/README.ko.md` 66줄; 보고서 경로 지침 `doc/AGENTS.md` 51줄.
- `doc/principal/documentation/spec-writing-guide.ko.md`: 824줄 파일의 전체 출력에는 도구의 출력 제한이 있었으므로 전수 계수에 넣지 않았다. 필수 기준은 별도로 확인한 `1–129`(129줄), `336–418`(83줄), `659–824`(166줄)이며 §1·§2.4·§4.3·§4.4·§9.3을 포함한다.
- `doc/principal/documentation/documentation-principles.ko.md`: 474줄 파일, 부분 출력 및 `215–376`(162줄) 중심으로 한국어 설명 원칙을 확인했다; 전수 읽음으로 세지 않았다.
- `doc/plan/c016-worklog/decisions.ko.md`: D-090 `1205–1210`(6줄), D-091 `1235–1241`(7), D-092 `1247–1251`(5), D-093 `1256–1261`(6), D-094 `1281–1285`(5), D-095 `1287–1292`(6), D-096 `1302–1306`(5), D-097 `1310–1314`(5), D-098 `1343–1352`(10), D-099 `1354–1359`(6), D-100 `1361–1364`(4), D-101 `1366–1378`(13), D-B96 `1123–1127`(5), D-B119 `1277–1279`(3), D-B120 `1294–1297`(4)을 읽었다. 주변 검색 일치 행은 추가로 확인했지만 전체 decisions 파일을 읽은 것으로 세지 않았다.
- `core/doc/spec/core/socket/README.ko.md`: `44–59`, `145–192`, `388–403`, `419–480`, `947–1077`, `1080–1106`, `1149–1160`, `1220–1258`, `1292–1305`의 관련 절을 읽었다. 영어는 `48–64`, `150–198`, `405–421`, `438–491`, `1050–1184`, `1277`, `1366–1409`, 그 외 검색 일치 행을 대조했다. 두 문서는 R1 소유 범위라 전수 검토하지 않았다.
- `core/doc/spec/core/socket/08-stream.ko.md`: `167–233`, `264–285`, `330–382`, `450–488`; 영어 `195–238`, `489–506` 및 검색 일치 행. `07-router.ko.md`·`.en.md`는 current-route reply와 reciprocal/standby 관련 검색 일치 행만 확인했다.
- `CONTRIBUTING.ko.md`: gate 관련 검색 일치 행과 `94–121`(28줄)을 읽었다. `core/tests/CMakeLists.txt`는 hotpath gate 등록 부근 `1098–1126`의 검색 일치 행만 확인했다.

### 구현 증거의 실제 읽은 구간

Core 구현 언어는 C++다. 아래는 문맥과 함께 읽은 구간이며 중복 읽기를 중복 계수하지 않았다. 단일 검색 일치 행은 이 표의 줄 수에 더하지 않았다. 출력 제한이 있었던 보조 구간은 전수 읽음으로 주장하지 않는다.

| 파일 | 문맥을 읽은 구간 | 구간 줄 수 |
|---|---|---:|
| `core/src/runtime/engine/asio/asio_zmp_engine.cpp` | 235–412, 434–520, 574–704 | 396 |
| `core/src/runtime/protocol/zmp_metadata.hpp` | 40–164 | 125 |
| `core/src/runtime/protocol/zmp_control.hpp` | 1–126 | 126 |
| `core/src/runtime/core/transport_pair_policy.hpp` | 1–49 | 49 |
| `core/src/runtime/sockets/common/socket_base_api.cpp` | 62–111, 285–424 | 190 |
| `core/src/runtime/sockets/router/router_admission.cpp` | 1–120, 160–510 | 471 |
| `core/src/runtime/sockets/router/router.cpp` | 26–102 | 77 |
| `core/src/runtime/sockets/dealer/dealer.cpp` | 24–43, 178–217, 289–316 | 88 |
| `core/src/runtime/sockets/internal/lb.cpp` | 260–360, 440–680 | 342 |
| `core/src/runtime/sockets/internal/dist.cpp` | 150–307 | 158 |
| `core/src/runtime/core/pipe.cpp` | 540–620, 916–971, 1250–1379, 1540–1735, 1765–1948, 2460–2710, 3230–3306, 3580–3710, 3905–3978 | 1,180 |
| `core/src/runtime/sockets/common/socket_send_complete.cpp` | 1–260 | 260 |
| `core/src/api/socket/socket_request_reply_submit_api.cpp` | 63–274 | 212 |
| `core/src/api/socket/socket_completion_queue_internal.cpp` | 1–95, 195–296, 360–431 | 269 |
| `core/src/api/socket/socket_request_reply_runtime_io.cpp` | 1110–1169, 1310–1408 | 159 |
| `core/src/api/socket/socket_request_reply_dispatch.cpp` | 124–171, 451–479 | 77 |
| `core/src/api/socket/socket_request_reply_pending_api.cpp` | 89–153 | 65 |
| `core/src/runtime/sockets/stream/stream.cpp` | 417–507, 525–635, 700–850, 1175–1205 | 384 |
| `core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp` | 15–91, 211–322 | 189 |
| `core/src/runtime/core/ctx_io_thread_registry.cpp` | 1–129 | 129 |
| `core/src/runtime/core/io_thread.cpp` | 1–106 | 106 |
| `core/src/runtime/core/ctx_runtime_resources.cpp` | 35–101 | 67 |
| `core/src/runtime/core/session_base.cpp` | 829–861 | 33 |
| `core/src/runtime/transports/tcp/asio_tcp_listener.cpp` | 239–267 | 29 |
| `core/src/runtime/core/ctx_physical_queue_registry.cpp` | 205–219, 830–928, 943–963 | 135 |
| `core/src/runtime/core/auto_hwm_policy.cpp` | 383–412 | 30 |
| `core/src/runtime/core/options_core_socket.cpp` | 111–144, 292–305 | 48 |
| `core/src/api/core/zlink_option.cpp` | 1–90 | 90 |
| `core/tests/perf/hotpath_gate.py` | 1–110, 156–238 | 193 |

`socket_base.hpp`의 receive-progress 선언 주변, `options.cpp`·`options.hpp`의 pending option 필드, `asio_poller.cpp`의 poll/run_for 루프, `asio_raw_engine.cpp`의 readiness, IPC·WS·TLS listener의 selector 호출은 검색·부분 문맥으로만 확인했다. 이 파일들의 전체를 읽었다고 주장하지 않는다.

지정 spec에서 건너뛴 파일은 없다. Core 전체 소스를 전수 읽지는 않았으며 ZMP handshake·lane-set admission, RAW packet pump, HWM/pipe·lb/dist, I/O thread 선택과 관련된 경로로 좁혔다. C++ 이외의 독립 구현 언어, bindings, Framework runtime은 이 job의 증거 범위 밖이므로 검증하지 않았다. `.ko.md`/`.en.md` 대조는 문서 parity이며 언어별 runtime parity 검증으로 세지 않았다. 공개 API repro 실행, 성능 수치와 전체 gate 상태는 read-only 제한 때문에 미검증이다.

## BLOCKERS

1. **B1 — D-096의 ‘재시도로 수렴’은 보장인가, 재시도 동작의 설명인가?** `core/doc/spec/core/protocol/01-zmp.ko.md:201–206` / 영어 `:206–212`는 수렴을 단정하지만 `doc/plan/c016-worklog/decisions.ko.md:1304–1306`은 두 intent의 계속된 충돌과 test 제거를 기록한다. `core/src/runtime/sockets/common/socket_base_api.cpp:66–98`은 incomplete pair를 RID로 결합하고 `:343–351`은 duplicate lane을 거부한다. 감독은 수렴 보장의 적용 전제를 무엇으로 승인했는가? 이 답 없이 보장을 약화하거나 새 wire ID·직렬화·retry 규칙을 제안하지 않는다.
2. **B2 — reciprocal HANDOVER에서 local RID와 peer RID가 같은 경우의 계약은 무엇인가?** Socket 공통 `core/doc/spec/core/socket/README.ko.md:166–168` / 영어 `:176–179`는 양쪽이 같은 방향을 택한다고 하지만 equal-RID의 전제·오류 결과를 그 절에서 정의하지 않는다. `core/src/runtime/sockets/router/router_admission.cpp:321–335`에서 `cmp == 0`이면 두 endpoint 모두 locally-initiated가 아닌 방향을 선호하여 서로 다른 physical 방향을 선택할 수 있다. 감독은 이 입력이 다른 계약으로 배제되는지, 아니면 spec gap인지 확인할 수 있는가? F-R2-6의 REJECT 우회와 별개 질문이며 tie-breaker를 임의로 추가하지 않는다.
3. **B3 — release 비교 gate는 모든 hot-path 변경에 필수인가, release 준비 때 필수인가?** `core/doc/spec/core/systems/10-hot-path.ko.md:99–100` / 영어 `:107–108`은 매 변경에 두 gate를 요구하지만 한국어 `:138` / 영어 `:150–151`은 release 준비 단계에 §5.2를 실행한다고 한다. `CONTRIBUTING.ko.md:97–108`은 성능에 손댄 변경의 release 비교를 요구한다. 감독은 실행 의무의 정확한 조건과 그 단일 소유 문서를 무엇으로 정할 것인가? `core/tests/perf/hotpath_gate.py:219–231`만으로 release 비교 실행 시점을 결정할 수 없으므로 임의 판정하지 않았다.
