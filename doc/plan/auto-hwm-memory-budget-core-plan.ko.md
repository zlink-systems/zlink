# Core Auto HWM 메모리 예산 적용 계획

## 문서 상태

- 상태: 구현 전 계획
- 대상: Core C API와 내부 전송 계층
- 공통 정책: [Auto HWM 메모리 예산 및 CCU 재계산 계획](auto-hwm-memory-budget-and-ccu-recalculation-plan.ko.md)
- 관련 문서:
  - [Bindings 적용 계획](auto-hwm-memory-budget-bindings-plan.ko.md)
  - [Framework 적용 계획](auto-hwm-memory-budget-framework-plan.ko.md)

## 1. 목적

Core는 인스턴스가 사용할 수 있는 하나의 messaging 메모리 예산을 받아 실제 연결 큐에 분배한다.
연결이 늘거나 줄면 각 큐의 HWM을 다시 계산한다. 이 예산은 정상 상태의 HWM 분배 기준이며 모든 큐의
실제 사용량을 묶어 차단하는 context hard cap이 아니다.
Framework job으로 전달한 message는 budget lease로 계속 Core가 회계한다.

Bindings와 Framework는 이 계산을 다시 수행하지 않는다.
Core가 프로필 비율 적용, 물리 큐 수 계산, HWM 분배, 실제 사용량 계측의 단일 기준점이다.

## 2. 현재 구현

### 2.1 프로필과 planning unit 기반 계산

| 프로필 | 일반 슬롯 | STREAM 슬롯 | 제어 슬롯 | 일반 cap | STREAM cap |
|---|---:|---:|---:|---:|---:|
| Compact | 64 | 8 | 8 | 256 | 32 |
| LowLatency | 128 | 16 | 16 | 512 | 64 |
| Balanced | 256 | 64 | 16 | 1024 | 128 |
| Throughput | 512 | 256 | 32 | 4096 | 512 |

기본 planning unit은 일반 소켓 4096바이트, STREAM 1024바이트다.
현재 planned SNDHWM과 RCVHWM은 선택 슬롯 수와 planning unit을 곱해 계산한다.
일반 소켓의 기본 결과는 Compact 256KiB, LowLatency 512KiB, Balanced 1MiB, Throughput 2MiB다.

근거:

- [프로필 표와 소켓 역할](../../core/src/runtime/core/auto_hwm_policy.cpp)
- [planning unit 선택과 socket plan 적용](../../core/src/runtime/sockets/common/socket_base.cpp)

### 2.2 연결 수 bucket의 비활성 상태

연결 수 bucket과 히스테리시스 구현은 남아 있다.
그러나 raw socket 호출 경로가 connection_bucket_enabled를 전달하지 않아 기본값 false가 사용된다.
현재 일반 Core socket에서는 연결 수 증가에 따라 HWM이 감소하지 않는다.

근거:

- [bucket 계산](../../core/src/runtime/core/auto_hwm_policy.cpp)
- [planner 선언의 기본 인자](../../core/src/runtime/core/auto_hwm_policy.hpp)
- [raw socket planner 호출](../../core/src/runtime/sockets/common/socket_base.cpp)

### 2.3 HWM의 의미

HWM은 예약하거나 미리 할당한 메모리 크기가 아니다.
각 pipe가 메시지를 받아들일 수 있는 accounted byte 상한이다.
실제 charge는 frame마다 payload와 msg_t 메타데이터를 합한 값이며, multipart 메시지는 모든 frame의 charge를 합산한다.

빈 pipe에는 HWM보다 큰 완전한 메시지 하나를 허용하는 liveness 예외가 있다.
LWM은 HWM의 절반을 올림한 값이므로 메시지 두 개를 별도로 보장할 필요는 없다.

근거:

- [pipe byte 회계와 admission](../../core/src/runtime/core/pipe.cpp)
- [socket HWM 사양](../../core/doc/spec/core/socket/README.ko.md)

### 2.4 재계산과 manual HWM

pipe attach와 detach는 context 재계산을 예약한다.
기본 debounce는 3000ms다.
현재 pending byte보다 작은 HWM으로 축소할 때는 적용을 미루지만, drain 시점에 자동 적용하지 않고 이후 재계산이 다시 발생해야 한다.

사용자가 SNDHWM 또는 RCVHWM을 설정하면 해당 방향은 manual 상태가 된다.
이 값은 특정 연결 하나를 선택하는 API가 아니라 socket의 모든 attached directional pipe에 반복 적용되는 값이다.

근거:

- [context 재계산](../../core/src/runtime/core/ctx_auto_hwm_recalc.cpp)
- [manual HWM 설정](../../core/src/runtime/sockets/common/socket_base_api.cpp)
- [attached pipe HWM 갱신](../../core/src/runtime/sockets/common/socket_base_lifecycle.cpp)

현재 inproc는 양 endpoint의 SNDHWM과 RCVHWM을 합쳐 physical ypipe HWM을 만들고 이후 갱신에도 boost를
적용한다. Completion lane에는 최소 256KiB floor가 적용된다. 이 두 legacy transform은 새 planner의
합계와 달라질 수 있으므로 memory-budget mode에서 명시적으로 우회해야 한다.

현재 DEALER와 ROUTER의 paired transport는 application lane과 completion lane을 만든다. Completion
lane은 정상 reply, error reply와 Router `completion-control` record를 운반한다. Core가 이 raw control의
내용을 만들지는 않지만 Framework mesh가 peer admission, liveness, relocation과 reply relay를 진행하기
위해 Router completion-control API를 실제 사용한다. 목표 정책에서는 이 Framework control을 application
routed lane으로 옮기고 Router completion-control API·envelope·handler를 제거한다. Completion lane은
terminal reply와 error reply 전용으로 유지하고 HWM 적용만 제거한다.

근거:

- [inproc endpoint HWM 합산](../../core/src/runtime/sockets/common/socket_base_endpoint.cpp)
- [pipe HWM boost 갱신](../../core/src/runtime/core/pipe.cpp)
- [completion lane HWM floor](../../core/src/runtime/core/transport_pair_policy.hpp)

### 2.5 현재 계측 범위

현재 monitor status의 planned/applied HWM과 in-flight 값은 byte지만 pending message field는 count다.
Core가 생성하는 기본 monitor socket은 4,096 event 깊이를 event charge로 곱해 byte HWM을 설정한다. 반면
일부 perf 경로의 `monitor-hwm`은 count를 `sizeof(msg_t)`만 곱하거나 값을 byte로 바로 사용해 의미가
일치하지 않는다.
context 전체의 planned HWM 합계, applied HWM 합계, 실제 accounted byte와 peak를 한 번에 조회하는 API는 없다.

근거:

- [socket monitor 집계](../../core/src/runtime/sockets/common/socket_base_monitor.cpp)
- [monitor 공개 구조체](../../core/include/zlink/eventing/api.h)

## 3. 목표 설정 모델

### 3.1 정규 계산 계약

입력 우선순위, profile 비율, 역할별 byte 경계, bounded water-filling과 budget 부족 처리는
[공통 계획의 Memory 입력 우선순위](auto-hwm-memory-budget-and-ccu-recalculation-plan.ko.md#4-memory-입력-우선순위)와
[Budget 분배](auto-hwm-memory-budget-and-ccu-recalculation-plan.ko.md#6-budget-분배)가 정규 사본이다.
Core 구현과 테스트는 그 식을 직접 사용하고 별도 수치표를 갖지 않는다.

Runtime memory hint와 감지한 finite hard limit이 모두 있으면 최솟값을 사용한다. Explicit memory limit
또는 manual Core budget이 감지된 hard limit보다 크면 `EINVAL`로 거절하고 조용히 clamp하지 않는다.

### 3.2 제안 context 옵션

정확한 상수 이름은 ABI 검토에서 확정하되 역할은 다음과 같이 분리한다.

| 옵션 | 단위 | 의미 |
|---|---:|---|
| AUTO_HWM_MEMORY_LIMIT_BYTES | byte | 사용자가 지정한 인스턴스 가용 메모리 |
| AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES | byte | managed runtime이 전달한 VM 또는 heap 한도 |
| AUTO_HWM_CORE_BUDGET_BYTES | byte | 프로필 계산을 건너뛰는 수동 Core 예산 |
| 기존 AUTO_HWM_PROFILE | enum | 예산 비율과 역할별 byte 하한·상한 선택 |
| 기존 AUTO_HWM_DEBOUNCE_MS | ms | topology 재계산 debounce |

0은 byte override가 설정되지 않았음을 뜻하며 무제한을 뜻하지 않는다. 기존 slot·message-unit 계산은
새 memory-budget 계산으로 직접 교체하고 policy 전환 mode를 두지 않는다.

Framework와 bindings의 공개 이름은 Core 의미에 맞춰 다음처럼 1:1로 대응한다.

| 공개 이름 | Core context option |
|---|---|
| CoreHwmMemoryLimitBytes | AUTO_HWM_MEMORY_LIMIT_BYTES |
| CoreHwmBudgetBytes | AUTO_HWM_CORE_BUDGET_BYTES |
| CoreHwmProfile | AUTO_HWM_PROFILE |

ApplicationHwmBytes와 ApplicationHwmProfile 공개 이름은 제공하지 않는다.

## 4. 물리 큐 기준 HWM 계산

### 4.1 Physical queue registry

분모는 논리 CCU나 socket 개수가 아니라 고유 physical ypipe 수다. Registry는 stable queue ID,
generation, 역할, 양 endpoint, manual 상태, provisional/committed byte, application lease byte,
applied HWM과 origin budget quota를 소유한다.

Inproc는 양 endpoint의 기존 HWM 합산값을 planner 입력으로 사용하지 않는다. 새 budget 정책에서는
공통 계획의 endpoint resolution 규칙으로 최종 physical ypipe cap을 한 번 계산하고 legacy HWM boost를
적용하지 않는다.

DEALER·ROUTER completion lane은 HWM을 계산하는 physical queue registry와 water-filling 분모에서
제외한다. Completion lane에는 auto HWM, manual SNDHWM·RCVHWM, 역할별 minimum·maximum, legacy
256 KiB completion floor와 Core budget reservation을 적용하지 않는다. 이를 public HWM 값 `0`으로
설정한 manual unlimited queue로 표현하지 않고, HWM admission 검사를 하지 않는 별도의 progress-lane
속성으로 표현한다.

### 4.2 원자적 minimum reservation

새 physical queue의 minimum reservation은 context budget lock 또는 동등한 CAS 안에서 확보한다.
성공 시점이 attach의 linearization point이며 reservation 뒤에만 queue를 publish한다. 비동기 network
연결이 실패하면 budget-rejected monitor event를 발생시키고 연결을 종료한다. 동기 inproc attach는
`ENOBUFS`를 반환한다.

Runtime budget 축소로 기존 minimum 합계가 새 budget보다 커지면 budgetInsufficient를 보고한다. 기존
message를 삭제하거나 minimum을 낮추지 않고 신규 연결을 거절한다. 기존 application pipe는 각자 적용된
HWM에 따라 독립적으로 admission과 drain을 진행하며 completion pipe는 영향을 받지 않는다.

Outbound terminal reply와 error reply만 completion lane으로 전송하며 planner reservation을 사용하지
않는다. Inbound application lease는 reply가 completion lane에 수용된 뒤 job 완료 시 반환한다.
Framework control과 일반 application send/request가 completion lane을 사용하는 것은 허용하지 않는다.

## 5. 연결 변화와 재계산

pipe attach로 분모가 증가하면 빠르게 재계산한다.
기존 queue의 HWM 축소가 필요하므로 일반 debounce보다 짧은 경로를 사용하거나 즉시 새 budget generation을 발행한다.
현재 pending byte보다 목표 HWM이 작으면 새 admission을 막고, drain으로 목표 이하가 되는 순간 deferred HWM을 적용한다.

pipe detach로 분모가 감소하면 남은 queue의 HWM을 늘릴 수 있다.
짧은 연결 진동으로 HWM이 계속 변하지 않도록 기존 debounce 또는 cooldown을 적용한다.

topology 또는 설정 변경의 계산 세대는 budgetGeneration으로 관리한다.
성능 측정 구간은 measurementEpoch로 분리한다.
연결 변화는 peak를 초기화하지 않으며 명시적 metrics reset만 measurement epoch를 증가시킨다.

## 6. Connection별 memory admission과 context 계측

### 6.1 context accounting

Core는 completion을 제외한 HWM-controlled physical queue와 Framework가 보유한 budget lease를 합산해
budgeted 현재·peak accounted byte를 관측한다. Completion pipe는 별도 current·peak로 관측하고 두 current를
더한 `totalMessagingAccountedBytes`도 보고한다. 이 합계들은 운영과 예산 튜닝을 위한 snapshot이며
message admission의 hard cap으로 사용하지 않는다.

- 각 frame charge는 payload와 `sizeof(msg_t)`를 포함한다.
- 마지막 frame에서 complete message의 charge를 확정하며 counter를 중복 증가시키지 않는다.
- Receiver가 frame을 제거하면 queue 소유 charge를 반환한다.
- Write 실패, multipart rollback, pipe 종료와 오류 경로는 provisional charge를 반환한다.
- Inproc 양 endpoint는 동일한 physical ypipe identity를 사용한다.
- Framework retained-credit receive는 charge를 제거하지 않고 owner만 application lease로 바꾼다.
- Job 완료, drop, cancel과 오류는 lease를 한 번 반환하며 applicationAccountedBytes가 감소한다.

`currentAccountedBytes`는 큰 message 한 건 예외, manual unlimited pipe 또는 재계산 중 drain 때문에
`effectiveCoreBudgetBytes`보다 클 수 있다. 초과 자체는 admission 오류가 아니며 snapshot에 그대로
나타난다.

### 6.2 Retained read credit

일반 receive는 현재처럼 완전한 message read byte를 `_published_bytes_read`에 반영하고 peer writer를
깨운다. Retained-credit receive는 message를 ypipe에서 제거하되 해당 charge를
`_published_bytes_read`에 반영하지 않는다. 기존 `bytesWritten - peerBytesRead` 계산이 Framework
lease byte를 origin queue in-flight로 계속 보게 한다.

Lease release는 origin queue actor에 deferred credit command를 보내고 다음 순서로 처리한다.

1. Lease ID와 queue generation을 검증한다.
2. Application accounted byte를 감소시킨다.
3. Origin queue의 published read credit을 증가시킨다.
4. 그 origin queue의 HWM과 waiter를 다시 검사한다.
5. Credit을 사용할 수 있는 session engine에 `restart_input()`을 전달한다.

Connection detach 시 outstanding lease가 있으면 registry entry를 retired tombstone으로 유지한다.
Retired queue는 새 topology의 분모에서는 제외하지만 context 계측에는 lease byte를 유지한다. 마지막
lease가 반환된 뒤에만 identity와 deferred state를 제거한다.

### 6.3 admission 조건과 connection 격리

일반 message는 대상 pipe의 현재 multipart 누적 charge와 그 pipe의 HWM만 확인한다.

~~~text
originQueueAccountedBytes + messageCharge <= originQuotaBytes
~~~

빈 pipe의 complete message 한 건은 이 식의 예외다. A pipe가 예외를 사용하거나 HWM에 도달해도 A만
inactive가 된다. B·C·D는 각자 HWM을 검사하므로 A의 pending byte와 관계없이 계속 진행한다. Context의
`currentAccountedBytes`가 budget을 넘었다는 이유로 B·C·D를 함께 차단하지 않는다.

Manual HWM도 해당 pipe의 조건만 결정한다. Context 합계는 manual·auto pipe 사용량을 모두 관측하지만
다른 pipe의 admission 조건으로 사용하지 않는다.

### 6.4 Decoder와 wake

Network decoder는 대상 origin pipe의 HWM credit을 기준으로 input을 계속 읽을지 결정한다. Credit 부족은
그 pipe의 engine input stop으로 연결한다. Credit 반환 시 해당 origin waiter만 다시 검사하고 성공할 때
`restart_input()`을 호출한다. 모든 connection을 깨우거나 하나의 context FIFO로 직렬화하지 않는다.

### 6.5 Completion lane은 HWM 없이 진행한다

Completion lane은 새로운 application 작업을 만드는 입력이 아니라 이미 수용한 작업을 완료시키는
경로다. 다음 record를 같은 completion pipe에서 처리한다.

- `reply_type`: pending request를 정상 완료하는 reply
- `error_reply_type`: pending request를 오류로 완료하는 reply

`completion_control_type`과 Router completion-control submit/handler API는 제거한다. Transport
handshake와 heartbeat는 transport engine이 처리하며 위 두 Core queue record와 구분한다. Framework
mesh control은 기존 Router routed message로 application lane을 사용한다.

Completion pipe의 send와 receive에는 byte HWM 검사, LWM wake와 empty-pipe oversize 예외를 적용하지
않는다. 유효한 complete record는 connection이 살아 있고 메모리 할당이 성공하면 크기와 현재 completion
pending byte에 관계없이 수용한다. 따라서 application lane이 HWM에 도달해도 다음 순서가 진행된다.

~~~text
application request 처리
→ terminal reply를 completion pipe에 수용
→ completion owner가 application recv와 독립적으로 drain
→ pending request·job 완료
→ application lease와 origin pipe credit 반환
→ application input backpressure 해제
~~~

Core는 completion record를 `CoreHwmBudgetBytes` 사용량이나 CCU 분모로 계산하지 않는다. 다만 제한을
없애는 것과 관측을 없애는 것은 다르므로 completion current·peak accounted byte와 pending message 수를
별도 snapshot으로 보고한다. 이 값은 진단에만 사용하며 completion admission을 중단하지 않는다.

Framework control을 application lane으로 옮겨도 Framework의 command allowlist, part 수와 payload 크기
검증은 유지한다. 이는 Core completion envelope 검증이 아니라 일반 routed payload를 받은 Framework가
수행하는 protocol validation이다.

### 6.6 Binding 비동기 전송을 위한 Core submit 경계

Core의 routed send와 request API는 `DONTWAIT`일 때 대상 RID pipe의 HWM을 한 번 검사하고
`BACKPRESSURED`를 즉시 반환한다. 다른 RID가 writable한지 기다리거나 socket 전체가 writable할 때까지
block하지 않는다. Multipart 실패는 partial record를 남기지 않고 caller가 전체 record를 재시도할 수
있도록 ownership을 복구한다.

Routed request는 wire에 request를 공개하기 전에 reply correlation을 준비한다. 수용에 실패하면 wire
record와 request state를 남기지 않고, 수용에 성공하면 Core가 reply 또는 timeout·disconnect terminal
결과를 정확히 한 번 전달한다. 이 관찰 가능한 결과만 Core 계약으로 두고 provisional state와 callback
등록 순서는 Core 내부 구현으로 둔다.

Binding은 이 nonblocking 결과와 Core의 전송 가능 상태 변화를 이용해 언어별 비동기 전송을 구현한다.
이를 위해 Core C ABI에 routed target readiness callback을 새로 추가한다. 기존
`zlink_send_ready_handler()`는 socket 전체에 하나 이상의 send 가능성이 생겼다는 신호라서 어느 RID가
회복됐는지 알 수 없다. A가 HWM에 막혀도 같은 socket의 B가 writable하면 이 callback이 먼저 소비될 수
있으므로 routed 비동기 admission의 wake 근거로 사용하지 않는다.

새 callback event는 최소한 target RID, transport pair ID와 generation, 그리고 `WRITABLE` 또는
`TERMINAL` 상태를 전달한다. Core는 HWM 때문에 inactive였던 application pipe가 credit을 회복해
`write_activated(pipe)`가 실행될 때 그 pipe의 identity로 `WRITABLE`을 발생시킨다. Pipe detach, socket
close와 context 종료에는 해당 identity의 `TERMINAL`을 발생시킨다. Callback은 재시도할 계기를 뜻하며
재시도 성공을 보장하지 않으므로 binding은 항상 같은 target으로 `DONTWAIT` submit을 다시 검사한다.

A pipe의 credit 회복은 A waiter를 재개한다. Pipe detach, socket close와 context 종료는 남은 waiter를
해당 terminal 상태로 정확히 한 번 깨운다. Timeout과 cancellation의 timer·상태 소유권은 binding에 둘 수
있지만 Core acceptance와 경합해 wire submit 또는 terminal 결과가 중복되어서는 안 된다. Target readiness
API는 모든 언어 binding이 사용하는 Core C ABI지만 Framework application API에는 노출하지 않는다.

## 7. 큰 메시지와 liveness

별도의 최소 두 메시지 보장은 두지 않는다. 기본 LWM은 HWM 절반의 올림값이지만 transport LWM hint가
더 작을 수 있다. Writer는 누적 반환 credit이 effective LWM 이상이거나 blocked 상태에서 queue가 완전히
drain됐을 때 깨어난다.

현재의 empty-pipe 예외를 connection별로 그대로 유지한다.

- Accounted charge가 per-pipe HWM보다 크고 대상 pipe가 비어 있으면 complete message 한 건을 수용한다.
- 수용한 큰 message 자체는 pending되지 않고, 같은 pipe의 다음 message부터 HWM에 막힌다.
- Context 전체의 oversize owner, allowance, debt waiter와 동시 수용 개수 제한을 추가하지 않는다.
- A의 oversize charge를 B·C·D의 HWM에서 차감하지 않으며 B·C·D에 backpressure를 전파하지 않는다.
- Auto HWM은 기존 unlimited `MAXMSGSIZE`를 budget 기반 상한으로 바꾸지 않는다.
- 사용자가 별도로 finite `MAXMSGSIZE`를 설정한 경우에만 기존 payload 검증 계약을 적용한다.

Multipart의 빈 queue 자격은 첫 frame 직전 상태로 고정한다. 이미 쓴 같은 message의 provisional frame은
자격을 없애지 않는다. 마지막 frame까지 complete message 하나로 회계하고 explicit rollback, close와
detach가 message를 폐기하면 provisional charge를 모두 반환한다.

## 8. manual HWM과 Core budget

manual SNDHWM과 RCVHWM은 기존 호환성을 유지한다.
해당 방향을 자동 per-pipe 계산에서 제외하고, 공통 계획의 endpoint resolution으로 얻은 최종 physical cap을
queue당 한 번만 manual reservation으로 차감한다. 양 endpoint가 모두 unlimited면 role maximum을
reservation으로 사용하고 aggregateHwmValid=false를 보고한다.

Manual pipe는 자신의 manual HWM만 admission에 적용하며 다른 pipe의 현재 사용량에는 영향받지 않는다.
manual HWM 0은 기존처럼 해당 pipe의 unlimited 의미를 유지한다. Snapshot에는
unlimitedManualQueueCount와 aggregateHwmValid를 표시해 유한한 HWM capacity 합계를 만들 수 없음을 알린다.

## 9. context 조회 API

### 9.1 snapshot 필드

| 필드 | 의미 |
|---|---|
| configuredMemoryLimitBytes | 명시적으로 설정한 가용 메모리 |
| runtimeMemoryLimitBytes | binding이 전달한 managed runtime 한도 |
| resolvedMemoryLimitBytes | Core가 최종 선택한 계산 입력 |
| configuredCoreBudgetBytes | 수동 Core 예산 |
| effectiveCoreBudgetBytes | 현재 pipe별 HWM 계산에 사용하는 Core 예산 |
| totalPlannedHwmBytes | Completion을 제외한 auto와 manual physical queue 목표 합계 |
| totalAppliedHwmBytes | Completion을 제외한 physical queue에 실제 적용된 HWM 합계 |
| manualReservedHwmBytes | manual 방향 reservation 합계 |
| coreQueueAccountedBytes | Completion을 제외한 physical queue의 provisional·committed charge |
| applicationAccountedBytes | Framework job이 retained lease로 보유한 charge |
| currentAccountedBytes | Core queue와 application-held charge의 합 |
| provisionalAccountedBytes | 아직 완전한 message로 commit되지 않은 frame charge |
| peakAccountedBytes | measurement epoch의 peak |
| completionCurrentAccountedBytes | HWM을 적용하지 않는 completion pipe의 현재 charge |
| completionPeakAccountedBytes | Measurement epoch에서 관찰한 completion charge의 최고값 |
| completionPendingMessageCount | Completion pipe에 남아 있는 complete record 수 |
| totalMessagingAccountedBytes | currentAccountedBytes와 completionCurrentAccountedBytes의 합 |
| monitorQueueAppliedHwmBytes | 모든 monitor event queue에 적용된 HWM byte 합계 |
| monitorQueueAccountedBytes | Monitor event queue의 현재 accounted byte 합계 |
| totalInstanceAppliedHwmBytes | totalAppliedHwmBytes와 monitorQueueAppliedHwmBytes의 합 |
| totalInstanceAccountedBytes | totalMessagingAccountedBytes와 monitorQueueAccountedBytes의 합 |
| oversizeAdmissionCount | 빈 pipe의 큰 message 예외를 수용한 누적 횟수 |
| largestOversizeMessageBytes | 예외로 수용한 complete message의 최대 accounted byte |
| activeDirectionalQueueCount | HWM 분모에 들어가는 중복 없는 application physical ypipe 수 |
| activeCompletionDirectionalQueueCount | HWM 분모에서 제외한 completion directional ypipe 수 |
| activeSendQueueCount | Completion을 제외한 HWM-controlled application 송신 queue의 관점별 수 |
| activeReceiveQueueCount | Completion을 제외한 HWM-controlled application 수신 queue의 관점별 수 |
| outstandingApplicationLeaseCount | 반환되지 않은 Framework lease 수 |
| retiredQueueCount | Lease 때문에 identity를 유지하는 detached queue 수 |
| deferredOriginCreditBytes | Lease 반환 전 publish를 미룬 origin read credit |
| unlimitedManualQueueCount | manual unlimited queue 수 |
| aggregateHwmValid | 합계 해석 가능 여부 |
| aggregateOverflow | 합계가 uint64 범위를 넘어 포화됐는지 여부 |
| budgetInsufficient | minimum 합계보다 예산이 작은 상태 |
| blockedRatioPpm | 최초 admission 시도 중 대상 pipe HWM block 비율 |
| budgetGeneration | 설정 또는 topology 계산 세대 |
| measurementEpoch | peak와 blocked 통계 측정 세대 |

### 9.2 Monitor queue byte 계약

`zlink_socket_monitor_open_options_t`에는 `uint64_t monitor_hwm_bytes`를 추가한다. 이 필드는 count가
아니며 Core는 양의 값을 변환 없이 내부 monitor PAIR의 SNDHWM과 RCVHWM에 적용한다. 0은 unlimited가
아니라 위 공통 계획의 Core 기본값 선택이다. Monitor queue는 Auto HWM을 끄고 application planner
registry와 CCU 분모에서 제외한다.

다음은 목표 public struct의 contract pseudocode다. 실제 ABI version과 reserved field는 header 검토에서
확정한다.

~~~c
typedef struct zlink_socket_monitor_open_options_t {
    zlink_socket_monitor_event_mask_t events;
    uint64_t monitor_hwm_bytes; /* 0 = Core default, positive = exact bytes */
} zlink_socket_monitor_open_options_t;
~~~

Socket monitor status에는 `snd_pending_bytes`와 `rcv_pending_bytes`를 추가한다. 기존
`snd_pending_msgs`와 `rcv_pending_msgs`는 관측용 count로 유지한다. Admission은 오직 pending/accounted
byte와 byte HWM으로 판단하며 count를 사용하지 않는다.

Legacy slot planner를 제거하면서 `auto_hwm_unit_budget_bytes`, `auto_hwm_size_cap`,
`auto_hwm_socket_message_slots`, `auto_hwm_effective_message_bytes`와
`auto_hwm_connection_bucket_*` status field도 제거한다. Byte 기반 planned/applied/deferred HWM,
in-flight byte, blocked ratio와 재계산 원인은 유지한다.

### 9.3 C API 초안

다음 선언은 **contract pseudocode이며 실제 public API가 아니다**. ABI 검토에서 exact interface를
확정해야 하며, 반환 형식은 기존 context configuration API 관례에 맞춰 `zlink_config_result_t`를 사용한다.

Routed 비동기 admission에 필요한 target readiness도 같은 ABI 검토에서 확정한다. 다음 callback은
socket-wide `zlink_send_ready_handler()`를 대체하지 않고 ROUTER·DEALER의 target별 비동기 대기에만 사용한다.
`peer_rid` storage는 callback 실행 중에만 유효하며 binding은 pending key 조회에 필요한 경우 값을 복사한다.

~~~c
typedef enum zlink_routed_send_ready_state_t {
    ZLINK_ROUTED_SEND_WRITABLE = 1,
    ZLINK_ROUTED_SEND_TERMINAL = 2
} zlink_routed_send_ready_state_t;

typedef struct zlink_routed_send_ready_event_t {
    zlink_routing_id_t peer_rid;
    uint64_t transport_pair_id;
    uint64_t transport_pair_generation;
    zlink_routed_send_ready_state_t state;
    int terminal_errno; /* WRITABLE이면 0 */
} zlink_routed_send_ready_event_t;

typedef void (*zlink_routed_send_ready_handler_fn) (
  void *subject,
  const zlink_routed_send_ready_event_t *event,
  void *userdata);

zlink_handler_result_t zlink_routed_send_ready_handler (
  void *socket,
  zlink_routed_send_ready_handler_fn handler,
  void *userdata);
~~~

Handler는 binding이 socket의 비동기 operation을 받기 전에 장기 등록한다. 같은 pipe의 여러 credit 반환은
하나의 `WRITABLE` event로 병합할 수 있고 불필요한 event도 허용하지만, HWM block 뒤 실제 writable 전이는
누락할 수 없다. Event generation이 현재 route generation과 다르면 binding은 stale event로 무시한다.
Handler 안에서 blocking submit을 실행하지 않고 binding scheduler에 대상 key만 전달한다.

~~~c
#define ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1 1u

typedef struct zlink_auto_hwm_budget_snapshot_t {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t budget_generation;
    uint64_t measurement_epoch;
    uint64_t configured_memory_limit_bytes;
    uint64_t runtime_memory_limit_bytes;
    uint64_t resolved_memory_limit_bytes;
    uint64_t configured_core_budget_bytes;
    uint64_t effective_core_budget_bytes;
    uint64_t total_planned_hwm_bytes;
    uint64_t total_applied_hwm_bytes;
    uint64_t manual_reserved_hwm_bytes;
    uint64_t core_queue_accounted_bytes;
    uint64_t application_accounted_bytes;
    uint64_t current_accounted_bytes;
    uint64_t provisional_accounted_bytes;
    uint64_t peak_accounted_bytes;
    uint64_t completion_current_accounted_bytes;
    uint64_t completion_peak_accounted_bytes;
    uint64_t completion_pending_message_count;
    uint64_t total_messaging_accounted_bytes;
    uint64_t monitor_queue_applied_hwm_bytes;
    uint64_t monitor_queue_accounted_bytes;
    uint64_t total_instance_applied_hwm_bytes;
    uint64_t total_instance_accounted_bytes;
    uint64_t oversize_admission_count;
    uint64_t largest_oversize_message_bytes;
    uint64_t active_directional_queue_count;
    uint64_t active_completion_directional_queue_count;
    uint64_t active_send_queue_count;
    uint64_t active_receive_queue_count;
    uint64_t outstanding_application_lease_count;
    uint64_t retired_queue_count;
    uint64_t deferred_origin_credit_bytes;
    uint64_t unlimited_manual_queue_count;
    uint32_t blocked_ratio_ppm;
    uint32_t flags;
    uint64_t reserved_u64[8];
} zlink_auto_hwm_budget_snapshot_t;

zlink_config_result_t zlink_ctx_get_auto_hwm_budget_snapshot (
  void *context,
  zlink_auto_hwm_budget_snapshot_t *snapshot);

zlink_config_result_t zlink_ctx_reset_auto_hwm_budget_metrics (
  void *context);
~~~

Caller는 abi_version과 자신이 할당한 struct_size를 설정하고 나머지를 0으로 초기화한다. Core는 자신이
아는 크기와 caller 크기 중 작은 범위만 기록한다. Header보다 짧거나 null이면 `EINVAL`, 지원하지 않는
version이면 `ENOTSUP`, 종료된 context면 `ETERM`을 반환한다. Reserved field는 0으로 유지한다.
Flags는 budgetPlanningActive, budgetInsufficient, aggregateHwmValid와 aggregateOverflow를 bit로 표현한다.

Metrics reset은 gauge를 지우지 않는다. `currentAccountedBytes`,
`completionCurrentAccountedBytes`, `completionPendingMessageCount`와 queue count는 현재값을 유지한다.
`peakAccountedBytes`와 `completionPeakAccountedBytes`는 각각 대응하는 current 값으로 재기준화한다.
Blocked·admission attempt counter, `oversizeAdmissionCount`와 `largestOversizeMessageBytes`는 0으로
초기화하고 `measurementEpoch`을 증가시킨다.

Snapshot은 한 budgetGeneration에 속한 일관된 registry view를 반환한다. Seqlock 재시도 또는 context
snapshot lock으로 queue 수, capacity와 accounted counter가 서로 다른 generation에서 섞이지 않게 한다.
합산 overflow는 UINT64_MAX로 포화하고 aggregateOverflow를 설정한다.

### 9.4 Framework retained-credit receive

Framework backend가 사용하는 Core ABI에는 opaque lease를 반환하는 receive variant를 추가한다.

~~~c
typedef struct zlink_hwm_budget_lease_t zlink_hwm_budget_lease_t;

int zlink_recv_with_hwm_budget_lease (
  void *socket,
  zlink_msg_t *message,
  zlink_hwm_budget_lease_t **lease,
  int flags);

void zlink_hwm_budget_lease_release (
  zlink_hwm_budget_lease_t **lease);

~~~

성공한 receive는 physical queue의 charge owner를 lease로 원자적으로 이전하므로 currentAccountedBytes를
변경하지 않는다. Lease 내부에는 originQueueId, originQueueGeneration과 accountedBytes가 들어간다.
일반 binding receive는 기존처럼 dequeue 시 credit을 즉시 반환한다. Framework만
retained variant를 사용한다.

Lease는 move-only이고 thread 간 전달할 수 있으며 release는 null-safe·idempotent하게 구현한다. Context
shutdown은 신규 이전을 막고 Framework job 취소 경로가 남은 lease를 반환하도록 기다린다. 강제 종료는
남은 lease를 invalid 처리한 뒤 counter를 한 번만 정리한다.

Retained receive는 이미 accounting된 queue message의 owner만 바꾸므로 항상 허용한다. Inbound lease는
원래 receive pipe의 credit을 유지한다. Reply와 error reply는 HWM이 없는 completion pipe에 수용하며,
성공한 submit 뒤 Framework job이 inbound lease를 반환한다. Oversize message를 context 전체에서
직렬화하지 않으므로 lease-to-send exchange API는 필요하지 않다.

## 10. Breaking change

- 기존 slot·message-unit Auto HWM을 memory-budget 계산으로 직접 교체한다.
- AUTO_HWM_POLICY_MODE와 legacy mode를 추가하지 않는다.
- AUTO_HWM_MSG_UNIT_BYTES는 제거하고 unknown option으로 거절한다.
- Framework ApplicationHwmBytes/Profile 이름과 호환 alias를 제공하지 않는다.
- 기존 socket monitor의 byte planned/applied/deferred, in-flight와 pending count는 유지하고 pending byte를
  추가한다. Slot·message-unit·connection-bucket planner 진단 필드는 제거한다.
- `zlink_socket_monitor_open_options_t.monitor_hwm_bytes`를 추가한다. 0은 Core 기본값이고 양의 값은
  변환 없는 byte HWM이다.
- DEALER·ROUTER completion lane의 HWM, LWM, 256 KiB floor와 Core budget reservation을 제거한다.
- Router completion-control submit·handler API, `completion_control_type` envelope과 관련 dispatch state를
  호환 alias 없이 제거한다.
- Completion progress lane에는 terminal reply와 error reply만 남긴다.
- Budget 부족은 새 connection의 minimum reservation을 거절한다. 기존 pipe는 각자의 새 HWM까지
  독립적으로 drain하며 다른 pipe의 admission을 함께 멈추지 않는다.
- Auto HWM을 끄면 마지막 applied 값을 유지하고 budgetPlanningActive=false를 보고한다.

## 11. 구현 순서

1. context 옵션과 versioned snapshot ABI 추가
2. OS, container, process memory limit resolver 추가
3. physical directional queue registry와 고유 identity 추가
4. bounded water-filling planner와 profile byte bounds 추가
5. attach 증가 즉시 축소, detach cooldown 재계산 추가
6. drain 시 deferred shrink 적용 추가
7. provisional frame accounting과 pipe별 oversize 한 건 admission 유지
8. Decoder의 origin pipe별 input stop과 wake 연결
9. Completion lane을 planner registry에서 제외하고 HWM admission·LWM wake·legacy floor 제거
10. Completion current·peak·pending과 total messaging accounted snapshot 추가
11. Retained-credit receive, deferred origin credit와 retired queue lifecycle 추가
12. manual reservation과 unlimited 진단 추가
13. Monitor open option, pending byte와 monitor·instance aggregate 추가
14. Legacy monitor count 설정 경로와 slot·bucket planner status field 제거
15. Routed send/request DONTWAIT의 target-local backpressure·multipart rollback 구현
16. RID·transport pair generation·writable/terminal 상태를 전달하는 routed target readiness C ABI와
    `write_activated(pipe)`·detach·close·context 종료 dispatch 구현
16. Framework routed-control 이관과 모든 binding callsite 제거가 완료된 통합 gate에서만 Router
    completion-control C API·export·envelope·handler·dispatch 제거

## 12. 검증 항목

- 입력 우선순위와 프로필 비율 1회 적용
- 일반/STREAM 하한, 상한과 manual reservation
- 혼합 일반/STREAM queue의 water-filling 합계가 data budget 이하인지 확인
- 정상 network 상태에서 applied HWM 합계가 effective budget 이하인지 확인
- runtime budget 축소의 budgetInsufficient 상태에서 applied capacity와 실제 accounted byte 구분
- inproc physical queue 중복 집계 방지
- inproc manual/auto/unlimited endpoint 조합의 최종 cap 확인
- Completion lane이 activeDirectionalQueueCount와 water-filling 분모에 포함되지 않는지 확인
- Completion lane에 auto/manual HWM, LWM과 legacy 256 KiB floor가 적용되지 않는지 확인
- Completion pipe의 pending byte가 증가해도 reply·error reply가 HWM 때문에
  `EAGAIN` 또는 backpressured 결과를 내지 않는지 확인
- Application lane이 HWM에 도달한 상태에서도 reply submit, completion drain, job 완료와 origin
  credit 반환이 이어지는지 확인
- Router completion-control C symbol, public header, envelope type, handler state와 dispatch가 제거됐는지 확인
- Framework control routed packet이 completion pipe로 들어오면 protocol error로 폐기되는지 확인
- Completion current·peak·pending은 관측하되 Core budget 사용량과 admission에는 반영하지 않는지 확인
- totalMessagingAccountedBytes가 application budget 사용량과 completion 사용량을 중복 없이 합산하는지 확인
- Metrics reset이 current·pending·queue count를 유지하고 두 peak를 각 current로 재기준화하며 epoch
  누적 counter만 0으로 만드는지 확인
- 명시 monitor HWM byte가 변환 없이 내부 monitor SNDHWM·RCVHWM에 적용되는지 확인
- monitor_hwm_bytes=0이 unlimited가 아니라 checked 기본 byte 값을 선택하는지 확인
- Monitor pending message count와 pending byte가 같은 queue 상태를 각 단위로 보고하는지 확인
- Monitor open/close가 application HWM 재분배와 activeDirectionalQueueCount를 바꾸지 않는지 확인
- totalInstanceAppliedHwmBytes와 totalInstanceAccountedBytes가 monitor queue를 정확히 한 번 합산하는지 확인
- Legacy `monitor-hwm` count 입력과 slot·bucket planner status field가 public header에서 제거됐는지 확인
- A RID의 DONTWAIT send/request가 즉시 BACKPRESSURED를 반환하고 B·C·D submit을 막지 않는지 확인
- A가 막히고 B가 writable일 때 socket-wide ready가 먼저 발생해도 A waiter wake가 유실되지 않는지 확인
- Routed readiness event의 RID·transport pair ID·generation이 실제 회복된 application pipe와 일치하는지 확인
- B의 writable 상태나 wake가 A의 target-ready event로 잘못 보고되지 않는지 확인
- Readiness 등록 전후 credit 회복 경쟁, pipe detach, socket close와 context 종료가 waiter를 정확히 한 번
  재개하거나 terminal 완료하는지 확인
- Multipart BACKPRESSURED에서 partial frame이 남지 않고 전체 message ownership이 복구되는지 확인
- Routed request의 callback state 설치와 wire publish가 원자적이며 BACKPRESSURED에서 둘 다 rollback되는지 확인
- Binding readiness 통합에서 A의 wake 대기가 B·C·D의 submit과 wake를 막지 않는지 확인
- activeSendQueueCount와 activeReceiveQueueCount가 completion을 제외하고
  activeCompletionDirectionalQueueCount가 completion만 별도로 세는지 확인
- 연결 증가 시 감소, 연결 감소 후 cooldown을 거친 증가
- drain 시 deferred shrink 즉시 적용
- Manual HWM이 다른 connection의 사용량과 독립적으로 적용되는지 확인
- Nonblocking EAGAIN, blocking timeout/close/detach와 pipe별 wake
- topology 변경 뒤 peak 유지와 explicit reset
- Incomplete multipart를 포함한 provisional/current/peak accounted byte와 rollback credit 확인
- Core queue에서 Framework lease로 owner를 이전할 때 total accounted byte 불변 확인
- Retained receive 후 origin queue in-flight와 published read credit이 job 완료까지 유지되는지 확인
- Hot connection lease가 다른 application connection의 HWM을 침범하지 않는지 확인
- Request handler가 application budget 포화 상태에서도 HWM 없는 completion lane으로 reply하고 완료되는지 확인
- A의 oversize message를 수용한 동안 B·C·D가 각자 HWM까지 계속 진행하는지 확인
- 여러 빈 pipe가 oversize message 한 건씩 동시에 수용할 수 있는지 확인
- Auto HWM이 unlimited MAXMSGSIZE를 유한한 값으로 바꾸지 않는지 확인
- Origin pipe credit이 부족할 때 그 pipe의 engine input만 멈추는지 확인
- Retired queue의 outstanding lease 반환 뒤에만 identity가 제거되는지 확인
- Job 완료, drop, cancel, timeout과 shutdown에서 lease를 정확히 한 번 반환
- Application credit이 가득 찼을 때 recv와 TCP sender가 block되고 job 완료 후 재개되는지 확인
- Blocking TCP, nonblocking EAGAIN, PUB drop policy와 inproc credit 동작을 각각 확인
- CPU 목표 70% 구간에서 budget별 throughput, p99 latency, blocked ratio 비교
