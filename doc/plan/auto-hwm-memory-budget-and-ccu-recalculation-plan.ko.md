# Auto HWM memory budget·CCU 재계산 공통 계획

> 상태: 구현 전 계획
>
> 이 문서는 현재 공개 계약이 아니다. Core, bindings와 Framework가 공유할 계산 기준과
> 책임 경계를 정의한다. 구성 요소별 API와 구현 순서는 아래 계획이 소유한다.

- [Core 구현 계획](auto-hwm-memory-budget-core-plan.ko.md)
- [Bindings 구현 계획](auto-hwm-memory-budget-bindings-plan.ko.md)
- [Framework 구현 계획](auto-hwm-memory-budget-framework-plan.ko.md)

## 1. 범위와 완료 결과

Auto HWM은 instance의 Core-managed messaging 정상 상태 byte budget을 정하고, 현재 directional pipe 수에
따라 pipe별 HWM을 자동으로 다시 계산한다. Core queue에서 Framework job으로 전달된 message도 원래
pipe의 credit 소유권만 이전하며 같은 pipe HWM에 남는다. Message의 평균 크기는 HWM을 결정하지 않는다.

구현이 끝나면 다음 결과를 관찰할 수 있어야 한다.

- Application은 기준 memory 또는 정확한 Core budget을 context에 설정할 수 있다.
- 정확한 Core budget이 없으면 Core가 기준 memory에 profile 비율을 한 번 적용한다.
- HWM-controlled auto physical queue가 추가되어 water-filling share가 줄면 기존 queue의 HWM도 감소한다.
- Queue가 제거되고 budget 여유가 있으며 profile 상한 미만인 queue는 안정화 시간이 지난 뒤 HWM이 증가한다.
- 기존 count 정책의 역할별 하한과 상한은 byte 단위로 유지한다.
- 빈 pipe는 HWM보다 큰 complete message 한 건을 처리할 수 있다.
- Context snapshot은 aggregate HWM capacity, Core queue byte와 application-held byte를 구분해 보고한다.
- Framework와 bindings는 Core budget을 별도로 만들지 않고 같은 Core context에 전달한다.

OS socket buffer, allocator overhead, thread stack과 managed heap의 다른 객체는 Core HWM의
accounted byte에 포함되지 않는다. `CoreHwmBudgetBytes`는 process RSS나 Core queue 사용량의 엄격한 상한이
아니라 pipe별 HWM을 계산하는 정상 상태의 분배 기준이다. 빈 pipe의 큰 message 한 건 예외와 재계산 중
drain 때문에 실제 accounted byte는 이 값을 넘을 수 있다.

## 2. 구성 요소 책임

| 구성 요소 | 결정하는 값 | 결정하지 않는 값 |
|---|---|---|
| Core | Profile 비율, 실제 pipe 수, 역할별 하한·상한, pipe별 HWM과 connection별 admission | Managed runtime의 heap limit 의미와 Framework instance topology |
| Bindings | Public type 변환, managed runtime limit 확인, Core context option과 snapshot mapping | Profile 계산, CCU 분배와 별도 Core budget |
| Framework | CoreHwm 설정 전달, message credit 보관·반환, Framework instance snapshot 노출 | Profile 계산, pipe 수, pipe별 HWM과 별도 application budget |

Core-managed messaging budget의 전달 경로는 하나다.

```text
Application configuration
→ Framework를 사용하면 Framework backend adapter
→ Binding context option
→ Core context
```

Framework와 binding은 같은 값을 각각 할당하지 않는다. Framework가 binding을 호출하면 binding은
값을 변경하지 않고 Core context option으로 전달한다.

Public 설정 이름은 모든 Framework와 binding에서 `CoreHwmBudgetBytes`, `CoreHwmProfile`과
`CoreHwmMemoryLimitBytes`로 맞춘다. 기존
`ApplicationHwmBytes`와 `ApplicationHwmProfile`은 제거하며 alias나 호환 mapping을 제공하지 않는다.

## 3. 값과 단위

| 값 | 단위와 범위 | 의미 |
|---|---|---|
| `effectiveMemoryBytes` | context당 byte | Profile 비율을 적용하기 전 기준 memory다. |
| `configuredCoreBudgetBytes` | context당 byte | Application이 지정한 정확한 Core-managed messaging budget이다. |
| `effectiveCoreBudgetBytes` | context당 byte | Core가 pipe별 HWM을 분배할 때 사용하는 정상 상태 budget이다. |
| `activeDirectionalQueueCount` | context당 queue 수 | Completion·monitor를 제외한 HWM-controlled physical directional ypipe를 registry identity당 정확히 한 번 센 값이다. 송신·수신 관점 수의 합이 아니다. |
| `plannedHwmBytes` | directional pipe당 byte | 현재 topology로 계산한 목표 HWM이다. |
| `appliedHwmBytes` | directional pipe당 byte | 현재 admission에 적용한 HWM이다. |
| `originQuotaBytes` | directional pipe당 byte | Queue와 그 queue에서 나온 application lease가 함께 사용할 budget quota다. |
| `coreQueueAccountedBytes` | context당 byte | Physical queue가 보유한 provisional·committed byte다. |
| `applicationAccountedBytes` | context당 byte | Framework job이 budget lease로 보유한 message byte다. |
| `currentAccountedBytes` | context당 byte | Core queue와 application-held byte를 합한 값이다. |
| `completionCurrentAccountedBytes` | context당 byte | HWM 없는 completion pipe가 보유한 현재 byte다. |
| `completionPeakAccountedBytes` | context당 byte | 현재 measurement epoch의 completion 최고 byte다. |
| `completionPendingMessageCount` | context당 message 수 | Completion pipe에 남은 complete record 수다. |
| `totalMessagingAccountedBytes` | context당 byte | Budgeted current와 completion current를 합한 전체 messaging byte다. |
| `activeCompletionDirectionalQueueCount` | context당 pipe 수 | HWM 분모에서 제외한 completion directional ypipe 수다. |
| `totalAppliedHwmBytes` | context당 byte | Completion을 제외한 HWM-controlled directional queue의 applied HWM 합계다. |

이 계획에서 반복해서 사용하는 내부 용어는 다음 뜻이다. 아직 공개 API로 확정한 용어가 아니며 구현
계약을 설명하기 위한 이름이다.

| 내부 용어 | 쉬운 뜻 |
|---|---|
| Origin quota | 특정 application directional pipe와 그 pipe에서 Framework로 옮긴 message가 함께 사용하는 HWM byte다. |
| Retained-credit lease | Framework가 message를 처리하는 동안 원래 pipe의 read credit 반환을 미루는 단일 소유 handle이다. |
| Completion progress lane | DEALER·ROUTER에서 terminal reply와 error reply만 운반하며 HWM을 적용하지 않는 completion lane이다. |

정상 상태의 분배와 connection 격리는 다음 불변식을 만족해야 한다.

```text
sum(planned auto HWM) + manualReservations
    <= effectiveCoreBudgetBytes

originQueueUsedBytes(queue) =
    physicalQueueBytes(queue)
    + applicationLeaseBytesFrom(queue)

originQueueUsedBytes(queue) <= originQuotaBytes(queue)
    // 단, 빈 pipe의 complete oversize message 한 건은 예외다.
```

두 번째 조건 때문에 Framework로 옮긴 message도 원래 connection의 HWM을 계속 점유한다. Hot connection이
Framework dequeue를 반복해 다른 application connection의 capacity를 침범할 수 없다.

`effectiveCoreBudgetBytes`에는 실제 사용량을 비교해 모든 connection을 함께 멈추는 전역 hard-cap
admission을 두지 않는다. A pipe가 자신의 HWM에 도달하면 A만 멈추고, B·C·D pipe는 각자 배정받은
HWM까지 계속 진행한다. A가 빈 상태에서 HWM보다 큰 message 한 건을 수용해도 이 초과분을 B·C·D의
credit에서 차감하지 않는다.

운영 환경의 CCU 하나가 항상 pipe 하나와 일치하지 않는다. 양방향 connection은 송신 queue와 수신
queue를 각각 가질 수 있고 PUB fanout은 subscriber마다 송신 queue를 만든다. 계산과 monitor는
논리 CCU가 아니라 실제 physical directional queue를 기준으로 한다.

## 4. Memory 입력 우선순위

Core는 다음 순서로 budget 입력을 선택한다.

1. 양수 `configuredCoreBudgetBytes`
2. Application이나 Framework가 전달한 양수 explicit memory limit
3. Managed binding이 전달한 양수 runtime memory limit과 Core가 감지한 finite hard limit의 최솟값
4. Core가 확인한 cgroup, Windows Job Object 또는 process address-space limit
5. Core가 확인한 시스템 전체 physical memory

`configuredCoreBudgetBytes`가 있으면 그 값을 그대로 사용한다.

```text
effectiveCoreBudgetBytes = configuredCoreBudgetBytes
```

정확한 budget이 없으면 선택한 memory에 profile 비율을 한 번 적용한다. Runtime limit과 감지된 hard
limit이 모두 있으면 `effectiveMemoryBytes = min(runtimeMemoryLimitBytes, detectedHardLimitBytes)`다.

```text
effectiveCoreBudgetBytes =
    (effectiveMemoryBytes / 100) × profilePercent
  + ((effectiveMemoryBytes % 100) × profilePercent) / 100
```

Memory-limit option의 `0`은 해당 입력을 사용하지 않고 다음 source를 확인한다는 뜻이다. Core budget과
memory limit에는 unlimited 의미를 두지 않는다. SNDHWM과 RCVHWM의 `0`이 뜻하는 unlimited와 별개다.

Core가 finite hard limit을 감지한 경우 explicit memory limit이나 manual Core budget이 그 limit보다 크면
설정을 `EINVAL`로 거절한다. 값을 조용히 clamp하지 않는다. 사용자가 잘못된 기준값을 바로 확인할 수
있어야 하며 Framework와 binding도 같은 오류를 그대로 전달한다.

## 5. Profile과 byte 경계

Core와 Framework가 노출하는 profile 이름은 다음 비율을 사용한다. 비율은 Core에서만 적용한다.

| Profile | Core-managed messaging budget 비율 |
|---|---:|
| `Compact` | 2% |
| `LowLatency` | 5% |
| `Balanced` | 10% |
| `Throughput` | 20% |

역할별 초기 하한과 상한은 기존 count 정책을 byte로 변환한 값이다. Perf와 memory test에서 값을
검증하되 하한·상한이라는 계약은 유지한다.

| Profile | 일반 data 하한 | 일반 data 상한 | STREAM 하한 | STREAM 상한 |
|---|---:|---:|---:|---:|
| `Compact` | 32 KiB | 1 MiB | 8 KiB | 32 KiB |
| `LowLatency` | 32 KiB | 2 MiB | 16 KiB | 64 KiB |
| `Balanced` | 64 KiB | 4 MiB | 64 KiB | 128 KiB |
| `Throughput` | 128 KiB | 16 MiB | 256 KiB | 512 KiB |

DEALER·ROUTER completion lane은 terminal reply와 error reply만 처리한다. 이 lane은 water-filling queue, CCU 분모, manual reservation과
`CoreHwmBudgetBytes` 사용량에서 제외한다. Auto HWM, manual SNDHWM·RCVHWM, LWM, 역할별
minimum·maximum과 empty-pipe oversize 규칙도 적용하지 않는다. 현재
`transport_pair_policy::completion_hwm()`의 256 KiB floor도 제거한다.

Router completion-control public API, envelope type과 handler는 제거한다. Framework의 hello,
admit/update/reject, liveness, relocation과 reply relay packet은 기존 Router routed send/receive를 사용해
application lane으로 이동한다. 따라서 Framework control도 대상 connection의 HWM과 정상적인
backpressure를 적용받는다.

## 6. Budget 분배

Core는 physical queue snapshot을 만든 뒤 다음 순서로 budget을 나눈다.

```text
manualReservationBytes =
    sum(final finite manual cap for each unique manual-controlled physical queue)
    + sum(profile role maximum for each fully-unlimited physical queue)

dataBudgetBytes = max(
    0,
    effectiveCoreBudgetBytes
      - manualReservationBytes)
```

Data queue에는 다음 bounded water-filling을 적용한다.

먼저 모든 auto queue minimum의 합을 checked arithmetic으로 계산한다. 합이 dataBudgetBytes보다 크면
water-filling을 실행하지 않고 budgetOversubscribed 처리로 이동한다. 따라서 아래 remaining 뺄셈은
항상 underflow 없이 수행된다.

```text
for each queue:
    planned[queue] = minimumHwmBytes(profile, role)

remaining = dataBudgetBytes - sum(planned)

while remaining > 0 and an unsaturated queue exists:
    share = max(1, remaining / unsaturatedQueueCount)
    for each unsaturated queue in stable queue-id order:
        grant = min(share, maximum[queue] - planned[queue], remaining)
        planned[queue] += grant
        remaining -= grant
```

나눗셈 remainder는 stable physical queue ID 순서로 1바이트씩 지급한다. 따라서 같은 topology와 입력은
항상 같은 결과를 만든다. 정상 상태의 필수 조건은 다음과 같다.

```text
sum(planned auto HWM) <= dataBudgetBytes
minimum[queue] <= planned[queue] <= maximum[queue]
```

새 queue의 하한까지 지급할 수 없으면 Core는 attach 전에 신규 connection admission을 거절한다.
실행 중 budget 축소로 기존 queue의 하한 합계가 budget보다 커지면 하한을 임의로 낮추지 않고
`budgetOversubscribed=true`를 보고한다. 이 과도 상태에서는 capacity 합계와 기존 accounted byte가 새
budget보다 일시적으로 클 수 있다. Core는 기존 message를 제거하지 않는다. 각 pipe는 새로 계산된 자기
HWM을 넘은 동안에만 신규 message를 막고 drain하며, 다른 pipe의 admission은 계속된다.
새 physical queue의 minimum reservation은 context budget lock 또는 동등한 원자적 CAS 아래에서
확보한다. Reservation 성공이 attach의 linearization point이며 queue를 다른 thread에 publish하기 전에
완료한다. 동시에 여러 attach가 발생해도 minimum을 중복 지급할 수 없다.

비동기 connect가 reservation을 얻지 못하면 해당 연결을 publish하지 않고 budget-rejected monitor
event를 발생시킨 뒤 연결을 종료한다. 동기 inproc attach는 `ENOBUFS`를 반환한다. 기존 연결은 유지한다.

## 7. Manual HWM과 Core budget

Manual SNDHWM과 RCVHWM은 socket에 설정되고 그 socket에 연결된 각 방향별 pipe의 최대값으로 적용된다.
Manual HWM은 Auto 분배가 그 pipe 값을 변경하지 않는다는 뜻이다. 실제 message admission은 다른
connection의 현재 사용량이 아니라 해당 pipe의 manual HWM만 검사한다.

Manual reservation 합이 Core budget보다 크면 Core는 Auto data budget을 0으로 만들고
`budgetOversubscribed=true`를 보고한다. 새 설정과 신규 connection은 `ENOBUFS`로 거절한다. Runtime
budget 축소로 이미 연결된 queue가 oversubscribed 상태가 되면 기존 연결은 유지하되 새 message
admission은 각 pipe의 적용 HWM에 따라 독립적으로 판단한다.

Manual HWM `0`은 기존처럼 해당 pipe가 unlimited라는 뜻이다. Capacity 합계를 계산할 때는 role maximum을
reservation으로 사용하고 `aggregateHwmValid=false`를 보고하지만, 이 진단용 reservation이 unlimited
pipe에 hard cap을 새로 만들지는 않는다.

Inproc의 같은 physical ypipe에는 양 endpoint 값을 더하지 않고 다음 endpoint resolution 규칙으로 최종 HWM 하나를
적용한다. 이 규칙은 memory-budget mode에만 적용한다.

| 송신 endpoint | 수신 endpoint | Physical ypipe 최종 cap |
|---|---|---|
| Auto | Auto | Water-filling 결과 |
| Finite manual | Auto | Manual cap |
| Auto | Finite manual | Manual cap |
| Finite manual A | Finite manual B | `min(A, B)` |
| Unlimited manual | Finite manual | Finite manual cap |
| Unlimited manual | Auto | Auto plan |
| Unlimited manual | Unlimited manual | Per-pipe unlimited, role maximum을 budget reservation으로 사용 |

Manual reservation은 이 최종 physical cap을 한 번만 센다. 양쪽이 unlimited면 합계가 무한해지지 않도록
해당 role의 profile maximum을 보수적인 reservation으로 사용하고 `aggregateHwmValid=false`를 보고한다.
같은 ypipe를 양 endpoint에서 각각 예약하지 않으며 legacy inproc HWM 합산과 boost는 memory-budget
mode에서 사용하지 않는다.

## 8. 큰 message 한 건의 진행성

Byte HWM을 엄격하게 적용하면 HWM보다 큰 유효 message가 빈 pipe에도 들어가지 못할 수 있다. Core는
빈 pipe에 complete message 한 건을 허용하는 기존 규칙을 유지한다. 최소 두 건 보장은 필요하지 않다.
기본 LWM은 `ceil(HWM / 2)`지만 transport가 더 작은 LWM hint를 설정할 수 있다. Writer는 누적 반환
credit이 effective LWM 이상일 때 깨어나며, blocked writer가 있고 queue가 완전히 drain된 경우에도
깨어난다. 따라서 진행성 계약은 정확한 message 개수가 아니라 빈 queue의 한 건 예외와 credit 반환이다.

큰 message의 진행성은 context 전체가 아니라 각 directional pipe가 독립적으로 보장한다.

- Accounted message charge가 pipe HWM보다 크고 그 pipe가 비어 있으면 complete message 한 건을 수용한다.
- 이 예외의 크기와 동시에 예외를 사용하는 pipe 수를 Auto HWM budget이 제한하지 않는다.
- A pipe가 예외를 사용하면 그 message 자체는 수용되고 A의 다음 message부터 HWM에 막힌다.
- A의 초과 accounted byte를 B·C·D의 HWM이나 credit에서 차감하지 않는다.
- B·C·D는 자신의 pipe HWM에 도달하기 전까지 계속 송수신한다.
- 큰 message가 소비되거나 rollback되면 A의 accounted byte가 반환되고 A가 다시 진행한다.
- `MAXMSGSIZE`는 사용자가 독립적으로 선택하는 기존 payload 검증 옵션이다. Auto HWM은
  `CoreHwmBudgetBytes`를 근거로 `MAXMSGSIZE`를 설정하거나 유한한 상한으로 바꾸지 않는다.

Multipart에서 빈 queue 여부는 첫 frame을 쓰기 직전 상태로 고정한다. 같은 message의 provisional frame은
그 message의 빈 queue 자격을 없애지 않는다. 마지막 frame까지의 accounted charge를 하나의 complete
message로 판단하고, write 실패나 explicit rollback, close와 detach는 그 message의 provisional byte를
모두 반환한다. 기존 multipart 재시도와 오류 계약은 바꾸지 않는다.

Framework lease가 oversize message를 보유하면 그 lease는 원래 A pipe의 HWM 점유를 job 완료까지
유지한다. 다른 pipe의 예외를 직렬화하는 context owner나 lease-to-send debt exchange는 추가하지 않는다.
Reply와 error reply는 HWM이 없는 completion progress lane에 수용하며, 성공한 submit 뒤 Framework가
inbound lease를 반환한다.

## 9. Aggregate 관측값

하나의 `totalHwmBytes`로 capacity와 실제 사용량을 함께 표현하지 않는다.

| 관측값 | 의미 | Manual budget 결정에 사용하는 방법 |
|---|---|---|
| `totalPlannedHwmBytes` | Completion을 제외한 auto·manual directional queue의 목표 capacity 합 | 재계산 결과 검증 |
| `totalAppliedHwmBytes` | Completion을 제외하고 실제 적용된 capacity 합 | 현재 Auto 결과를 같은 topology에서 재현할 후보 |
| `coreQueueAccountedBytes` | Completion을 제외한 HWM-controlled queue가 보유한 byte | Transport 적체 확인 |
| `applicationAccountedBytes` | Framework job이 lease로 보유한 byte | Application 처리 적체 확인 |
| `currentAccountedBytes` | Core queue와 application-held byte를 합한 budgeted 현재값 | Core budget 사용률 계산 |
| `peakAccountedBytes` | 측정 구간에서 관찰한 budgeted 최고 사용량 | Manual budget 상한 후보 |
| `completionCurrentAccountedBytes` | HWM 없는 completion pipe의 현재 byte | Progress lane 적체 확인 |
| `completionPeakAccountedBytes` | 측정 구간의 completion 최고 byte | Completion 이상 징후 확인 |
| `completionPendingMessageCount` | Completion pipe에 남은 complete record 수 | Drain 진행 확인 |
| `totalMessagingAccountedBytes` | currentAccountedBytes와 completionCurrentAccountedBytes의 합 | 현재 전체 messaging byte 확인 |
| `monitorQueueAppliedHwmBytes` | 모든 monitor event queue에 적용된 byte HWM 합 | 진단 queue capacity 확인 |
| `monitorQueueAccountedBytes` | Monitor event queue가 현재 보유한 accounted byte | Monitor 소비 지연 확인 |
| `totalInstanceAppliedHwmBytes` | totalAppliedHwmBytes와 monitorQueueAppliedHwmBytes의 합 | HWM이 있는 instance queue capacity 확인 |
| `totalInstanceAccountedBytes` | totalMessagingAccountedBytes와 monitorQueueAccountedBytes의 합 | Completion과 monitor를 포함한 현재 Core queue byte 확인 |
| `blockedRatioPpm` | 최초 admission 시도 중 대상 pipe HWM 때문에 block된 비율 | Budget 부족 여부 판단 |
| `oversizeAdmissionCount` | 빈 pipe의 큰 message 예외를 수용한 누적 횟수 | Peak 원인 구분 |
| `largestOversizeMessageBytes` | 예외로 수용한 complete message의 최대 accounted byte | 큰 message 영향 확인 |

### 9.1 Monitor queue HWM은 byte만 사용한다

Monitor event queue는 application Auto HWM water-filling, profile 비율과 CCU 분모에서 제외한 진단용
queue다. 설정과 admission 단위는 예외 없이 byte다. Public option은 `monitorHwmBytes`, perf CLI는
`--monitor-hwm-bytes`, 환경 변수는 `PERF_MONITOR_HWM_BYTES`와
`PERF_MULTI_MONITOR_HWM_BYTES`를 사용한다. 기존 `monitor-hwm` count 이름과 환경 변수는 alias 없이
제거한다.

명시 값이 없을 때 Core 기본값은 기존 4,096 event 깊이와 동등한 byte 값으로 한 번만 계산한다.

```text
defaultMonitorHwmBytes =
    checkedMultiply(
        4096,
        sizeof(socket_monitor_internal_event_t) + sizeof(msg_t))
```

외부 입력은 event count로 다시 환산하지 않고 그대로 monitor SNDHWM과 RCVHWM byte에 적용한다.
`monitorHwmBytes=0`은 Core 기본값 선택이며 monitor queue unlimited를 뜻하지 않는다. 명시 값은 1 이상이어야
하고 overflow는 설정 오류로 거절한다.

Pending message count는 queue 동작을 설명하는 관측값으로 유지하고 같은 snapshot에 pending byte를 함께
제공한다. Count는 HWM admission이나 byte 변환의 입력으로 사용하지 않는다. 기존 slot planner 진단값인
`auto_hwm_unit_budget_bytes`, `auto_hwm_size_cap`, `auto_hwm_socket_message_slots`,
`auto_hwm_effective_message_bytes`와 connection bucket 진단 필드는 새 memory-budget 정책에서 제거한다.

Monitor queue를 열거나 닫아도 application queue HWM을 재분배하지 않는다. 따라서
`totalAppliedHwmBytes`와 `currentAccountedBytes`에는 monitor queue를 넣지 않고 위 monitor 전용 필드와
instance 전체 합계에서만 더한다. Inproc 양 endpoint option을 단순 합산하지 않고 고유한 physical monitor
directional queue를 한 번씩 센다.

```text
totalInstanceAppliedHwmBytes =
    totalAppliedHwmBytes + monitorQueueAppliedHwmBytes

totalInstanceAccountedBytes =
    totalMessagingAccountedBytes + monitorQueueAccountedBytes
```

HWM-controlled physical queue는 context 안에서 고유한 accounting identity를 가진다. 각 frame을 ypipe에 기록할 때
payload와 `sizeof(msg_t)`를 포함한 provisional byte를 context 계측에 원자적으로 더한다.
마지막 frame에서는 provisional 합계를 committed message로 전환하되 counter를 다시 증가시키지 않는다.
Receiver가 frame을 제거하면 해당 charge를 반환한다. Write 실패, multipart rollback, pipe 종료와 오류
경로는 남은 provisional credit을 반드시 반환한다.

Framework용 receive는 queue에서 byte를 제거할 때 origin pipe credit을 반환하지 않는다. 같은 charge의
budget lease를 message와 함께 넘기고 owner를 Core queue에서 application으로 원자적으로 바꾼다.
Framework는 message가 queued 또는 active job인 동안 lease를 보유하고 완료, drop, cancel 또는 오류로
수명이 끝날 때 Core에 반환한다.

Lease는 originQueueId, originQueueGeneration과 accountedBytes를 보존한다. Owner를
application으로 바꿀 때 physical queue의 read credit을 writer에 publish하지 않는다. 따라서 다음
origin quota도 유지된다.

```text
originQueueUsedBytes =
    bytes not yet dequeued
    + bytes retained by Framework jobs from that queue
```

Job 완료로 lease가 반환될 때 origin queue의 deferred read credit을 반환하고 해당
session input waiter를 깨운다. Connection이 먼저 detach되면 queue identity를 retired 상태로 유지하고
outstanding lease가 모두 반환된 뒤 제거한다. Retired lease byte는 context 사용량에는 남지만 새
topology의 다른 pipe HWM에서 차감하지 않는다.

```text
currentAccountedBytes =
    coreQueueAccountedBytes + applicationAccountedBytes

totalMessagingAccountedBytes =
    currentAccountedBytes + completionCurrentAccountedBytes
```

`currentAccountedBytes`는 oversize 한 건 예외와 재계산 중 drain 때문에
`effectiveCoreBudgetBytes`보다 클 수 있다.

Credit owner 이전은 합계를 바꾸지 않는다. 따라서 Framework가 Core queue를 drain해도 원래 pipe의
credit이 잘못 회복되지 않는다. Job 완료로 lease가 반환될 때만 그 pipe의 recv admission과 sender backpressure를
해제한다.

### 9.2 Allocation 전 provisional credit

Backpressure 검사는 payload buffer를 할당한 뒤가 아니라 크기를 확인한 직후 수행한다. Decoder는 frame
header에서 길이를 확인하면 origin queue quota의 provisional credit을 먼저 확보한다.
Credit을 얻은 뒤에만 payload body buffer를 할당하고 읽는다.
빈 origin pipe의 complete oversize message는 §8의 한 건 예외로 이 quota 검사를 통과한다.

```text
frame length decode
→ origin provisional credit acquire
→ payload buffer allocation
→ body decode
→ physical queue commit
```

Credit이 없으면 engine input을 중단하고 해당 origin waiter를 등록한다. Lease release 또는 pipe
read credit이 회복되면 waiter를 공정한 순서로 깨워 `restart_input()`을 호출한다. Header와 codec의
고정 working buffer, OS socket buffer와 allocator overhead는 이 budget 밖이므로 별도 RSS reserve로
관리한다.

Socket별 send·receive snapshot을 합산해 context 값을 만들지 않는다. Inproc 양 끝에서 같은 queue를 두
번 세거나 approximate receive 값을 context 진단값에 사용하는 것을 방지한다.

`activeDirectionalQueueCount`는 HWM 분모에 들어가는 application ypipe 수다. `activeSendQueueCount`와
`activeReceiveQueueCount`도 completion을 제외한 HWM-controlled application queue만 각 관점에서 센다.
같은 ypipe가 양쪽에 나타날 수 있으므로 둘을 합산해 physical queue 수로 사용하지 않는다.
`activeCompletionDirectionalQueueCount`는 HWM 분모에서 제외한 completion directional ypipe 수를 별도로
센다. Applied 합계가 unlimited 때문에 유한하지 않으면
`aggregateHwmValid=false`와 `unlimitedManualQueueCount`를 함께 반환한다. 합산 overflow는 포화값과
`aggregateOverflow=true`로 보고한다.

`blockedRatioPpm = floor(firstBlockedAdmissionAttempts × 1,000,000 / totalAdmissionAttempts)`다. 같은
send 호출의 wake 재시도는 분자와 분모에 다시 세지 않는다. 대상 pipe HWM block만 포함하고 transport
I/O 대기나 context aggregate 사용량은 포함하지 않는다.

Topology를 식별하는 `budgetGeneration`과 성능 측정 구간을 식별하는 `measurementEpoch`은 별도 값이다.
Connection 변화는 budget generation을 바꾸지만 peak를 초기화하지 않는다. Application이 metrics reset을
요청하면 measurement epoch을 증가시키고 gauge인 current·pending·queue count는 유지한다.
`peakAccountedBytes`와 `completionPeakAccountedBytes`는 각각 reset 순간의 current 값으로 재기준화해 같은
epoch에서 peak가 current보다 작아지지 않게 한다. Blocked attempt·전체 admission attempt,
`oversizeAdmissionCount`와 `largestOversizeMessageBytes` 같은 epoch 누적값은 0으로 초기화한다.

## 10. Framework credit 소유권

Framework에는 별도의 HWM budget과 profile을 두지 않는다. `CoreHwmBudgetBytes`와 `CoreHwmProfile`은
binding을 통해 Core option을 설정하는 public facade다. Framework ingress의 queued/active payload는
별도 limit으로 계산하지 않고 Core가 발급한 budget lease로 제한한다.

특정 origin pipe의 credit이 소진되면 그 pipe의 transport ingress admission이 중단되고 TCP backpressure로 이어진다.
이미 Core queue에서 accounting된 message를 Framework job으로 옮기는 retained receive는 합계를 늘리지
않으므로 계속 허용한다. Job 완료로 application lease가 반환되면 Core가 transport recv와 blocked
sender를 깨운다.

Completion progress lane은 이 budget과 backpressure의 대상이 아니다. Application lane이 가득 차도
terminal reply와 error reply는 HWM 없는 completion pipe에 수용되고 Core의 completion owner가
application recv와 독립적으로 drain한다. Framework mesh control은 application lane에서 일반 routed
packet으로 처리하며 장시간 backpressure로 heartbeat timeout이 나면 기존 topology 규칙에 따라 peer를
제거한다.

### 10.1 비동기 최초 제출과 RID 격리

Framework는 공유 ROUTER의 send, request와 Framework control을 binding의 비동기 전송 API로 호출한다.
Binding은 대상 pipe가 HWM에 도달했어도 호출한 언어의 event loop나 runtime worker thread를 점유하지 않고
제어권을 즉시 scheduler에 돌려준다. HWM이 회복될 때까지 기다리는 동안 socket 전체 submit lock도
보유하지 않는다. Completion lane의 terminal reply와 error reply에는 이 대기가 적용되지 않는다.

A RID가 backpressure 상태이면 A로 보내는 비동기 operation만 대기한다. 같은 socket을 사용하는 B·C·D
operation은 Core에 독립적으로 제출되어 진행할 수 있어야 한다. `Task`, `CompletableFuture` 또는 `Promise`를
반환한다는 사실만으로 이 조건을 만족한 것으로 보지 않는다. 반환 객체를 만들기 전에 native blocking
submit을 호출해 runtime thread를 멈추는 구현은 허용하지 않는다.

Backpressure 대기와 재개는 binding 비동기 전송 계약의 일부다. Binding은 Core의 nonblocking submit 결과와
전송 가능 상태 변화를 이용해 기다리던 operation을 재개한다. Framework는 이를 다시 구현하는
`pendingByTarget`, RID별 deque, ready ring 또는 고정 주기 polling을 두지 않는다. Binding 내부 구현에도
Application이 설정하는 별도 retry queue 용량이나 queue-full 오류를 추가하지 않는다.

Core와 binding은 대상 pipe의 수용 실패와 readiness 대기 등록 사이에서 wake를 잃지 않아야 한다. A가
HWM에 막혀도 같은 socket의 B가 writable이면 socket-wide send-ready가 먼저 소비될 수 있으므로, binding은
waiter를 등록한 뒤 수용을 시도하거나 등록 뒤 A pipe 상태를 다시 검사한다. A pipe의 credit 회복, detach,
socket close와 context 종료는 기다리는 A operation을 정확히 한 번 깨운다. Timeout과 cancellation은 같은
operation을 기존 terminal 결과로 완료한다. 이 내부 보장을 Framework가 readiness callback이나 재시도
queue로 다시 구현하지 않는다.

One-way send의 비동기 완료는 Core가 complete multipart를 수용한 시점이다. Request의 비동기 operation은
최초 수용을 기다린 뒤 기존 request/reply lifecycle을 계속 수행하며 reply 또는 기존 timeout·disconnect·
cancellation 결과로 완료된다. 최초 deadline은 backpressure 대기 중에도 연장되지 않는다. Request가 wire에
보이기 전 reply correlation이 준비되어야 한다는 원자성은 Core와 binding이 보장하며 Framework가 callback
등록 상태기를 따로 만들지 않는다.

Framework control도 send·request와 같은 binding 비동기 전송 계약을 사용한다. Control command라는 이유로
별도 lane, 우선순위 queue, 우회 queue나 polling loop를 만들지 않는다. Heartbeat timeout과 peer 제거는
전송 queue 정책이 아니라 기존 topology lifecycle의 결과다.

전송별 관찰 결과는 다음과 같다.

| 전송·API | Budget 소진 시 결과 |
|---|---|
| Core direct blocking TCP send | Kernel과 Core buffer가 찬 뒤 block |
| Nonblocking send | `EAGAIN` |
| PUB 계열 | Socket의 기존 정책에 따라 block 대신 drop 가능 |
| Inproc | TCP window 없이 대상 pipe credit으로 직접 block |

위 blocking 행은 Core 직접 API의 동작이다. Framework가 공유 ROUTER에 제출하는 send·request·control은
항상 nonblocking 경로를 사용한다.

TCP backpressure는 OS receive/send buffer 때문에 즉시 나타나지 않는다. Budget 검증은 Core-managed
message backlog를 제한하며 process RSS나 전체 managed heap의 엄격한 상한을 뜻하지 않는다. Handler가
생성한 object graph, payload 사본과 job 완료 뒤 user code가 보관한 reference는 별도 계측 대상이다.

## 11. Breaking change와 구현 순서

기존 slot·message-unit Auto HWM과 Framework `ApplicationHwmBytes/Profile`은 새 memory-budget 정책으로
직접 교체한다. Legacy policy mode, deprecated alias와 호환 mapping을 제공하지 않는다. 잘못된 이전
이름은 configuration validation에서 unknown option으로 거절한다.

구현 순서는 다음과 같다.

1. Core budget option, exact accounting, application lease와 snapshot을 구현한다.
2. Core가 connection 증가·감소에 따라 HWM을 다시 계산하도록 한다.
3. Bindings가 CoreHwm option, retained-credit receive와 snapshot을 손실 없이 노출한다.
4. Bindings가 최초 Core 수용 대기까지 포함하는 비동기 send/request 계약을 구현하고 언어 runtime thread와
   socket 전체 submit lock을 점유하지 않게 한다.
5. Framework의 공용 pending deque, RID별 retry scheduler와 고정 주기 polling을 제거하고 send·request를
   binding 비동기 전송 API로 통합한다.
6. Legacy completion-control API가 아직 존재하는 동안 Framework control packet을 같은 binding 비동기
   전송 API와 기존 Router routed send/receive·application pump로 옮긴다.
7. 모든 Framework와 binding 호출 경로가 이관된 통합 gate에서만 Core와 bindings의 Router completion-control
   API·envelope·handler를 제거한다.
8. Managed bindings가 runtime memory limit을 Core에 전달한다.
9. Framework가 기존 application budget을 제거하고 lease를 job 수명과 연결한다.
10. Core·bindings·Framework와 perf 도구의 monitor HWM 입력을 byte로 통일하고 legacy count 진단 필드를
   제거한다.
11. Perf test로 profile 하한·상한, completion 진행성과 end-to-end backpressure를 검증한다.

## 12. 공통 완료 조건

- Profile 비율은 Core에서 한 번만 적용된다.
- Manual Core budget은 Framework와 binding에서 변경되지 않는다.
- ApplicationHwm 이름과 별도 Framework budget은 존재하지 않는다.
- Application queue와 manual reservation의 합은 Core budget 안에서 계산된다.
- Completion progress lane은 HWM capacity 합계, water-filling 분모와 Core budget reservation에서 제외한다.
- Terminal reply와 error reply만 completion pipe에서 HWM 없이 진행한다.
- Router completion-control API·envelope·handler는 Core와 모든 binding에서 제거한다.
- Framework hello/admission/liveness/relocation/relay packet은 application routed lane과 connection별 HWM을 사용한다.
- Core budget은 pipe별 HWM을 계산하는 정상 상태 분배 기준이며 context hard cap으로 사용하지 않는다.
- Oversize exception은 빈 directional pipe마다 complete message 한 건을 독립적으로 허용한다.
- 한 pipe의 oversize byte는 다른 pipe의 HWM과 credit을 줄이거나 backpressure를 전파하지 않는다.
- Auto HWM은 oversize allowance나 `MAXMSGSIZE` 상한을 새로 만들지 않는다.
- Context accounted byte는 provisional frame과 application-held lease를 포함해 message마다 정확히 한 번 계산된다.
- Application lease는 origin directional queue HWM을 job 완료까지 유지한다.
- Reply가 completion pipe에 수용된 뒤 Framework job이 inbound application lease를 반환한다.
- Decoder는 payload allocation 전에 provisional credit을 확보한다.
- Budget generation과 measurement epoch이 분리된다.
- Monitor HWM 설정은 모든 계층에서 byte이며 count 입력이나 암시적 변환이 존재하지 않는다.
- Monitor pending count는 진단으로만 유지하고 pending byte를 함께 제공한다.
- Monitor queue는 application budget 분배에서 제외하고 monitor·instance aggregate에 별도로 합산한다.
- Send·request·Framework control은 binding의 같은 비동기 최초 수용 계약을 사용한다.
- A RID의 backpressure 대기는 같은 socket의 B·C·D operation과 언어 runtime thread를 막지 않는다.
- A가 막히고 B가 writable인 상태에서도 A의 readiness wake가 유실되지 않는다.
- 대상 pipe의 credit 회복·detach, socket close, context 종료와 timeout·cancellation은 대기 operation을
  정확히 한 번 재개하거나 terminal 완료한다.
- Framework에는 `pendingByTarget`, RID별 deque, ready ring 또는 고정 주기 submit polling이 없다.
- Control command에는 별도 lane, 우선순위 queue나 우회 queue를 두지 않는다.
- Request의 callback lookup state와 wire publish는 linearizable하며 `BACKPRESSURED` 뒤에는 wire·Core pending·Core callback 소유권이 모두 남지 않는다.
- Completion reply와 error reply는 비동기 application-lane 수용 대기의 대상이 아니다.
- Core, bindings와 Framework가 같은 입력과 snapshot 의미를 사용한다.
