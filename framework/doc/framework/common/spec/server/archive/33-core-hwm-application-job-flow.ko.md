---
title: "Core byte HWM과 Application job flow"
---

# Core byte HWM과 Application job flow

[스펙 목차](../README.ko.md) · [이전: Framework 오류 모델](32-framework-error-model.ko.md)

> **이 장이 정의하는 것** — Core queue의 byte HWM과 Framework Application Job Queue의
> job-count pressure를 분리하는 이유, 두 제어가 만나는 유일한 runtime 경계와 ordinary ingress의
> permit 순서.

## 1. 설계 의도

Core와 Framework는 서로 다른 과부하를 관찰한다.

- Core는 transport queue가 현재 보유한 frame byte를 알고 connection·payload 크기로 인한 queue
  memory burst를 제한한다.
- Framework는 handler가 시작되기 전에 대기하는 application job 수를 알고 application 처리 속도보다
  빠른 유입을 제한한다.

한 계층의 값을 다른 계층의 counter로 복사하면 책임이 겹친다. Core가 handler 처리 능력을 추측하거나
Framework가 socket queue byte를 계산해서는 안 된다. 따라서 두 한도는 설정, profile, 단위, 계상
경계와 관측값을 공유하지 않는다.

Core HWM은 전송 경로의 마지막 안전장치다. Framework pressure가 미리 remote traffic을 줄이더라도
이미 remote Core queue, OS buffer, network와 local Core queue에 들어간 data가 있다. 반대로 Core
queue가 비어 있어도 handler job이 오래 대기할 수 있다. 두 보호가 모두 필요하지만 같은 보호는 아니다.

## 2. 독립된 두 capacity authority

| Authority | 제한하는 것 | 계상 또는 획득 | 반환 |
|---|---|---|---|
| Core byte HWM | Core application-direction queue가 현재 보유한 physical-frame charge | Core queue가 frame을 소유할 때 | Receive dequeue 등으로 Core queue가 frame 소유권을 내놓을 때 |
| Framework Application Job Queue | Host가 callback 시작 전에 수용한 application job permit 수 | Ordinary ingress를 receive·claim하기 직전에 reservation하고 handler turn으로 전환할 때 | Callback의 실제 첫 instruction 직전 또는 callback 전 terminal에서 |

Core frame charge는 Core 계약이 정한 payload와 metadata byte를 포함한다. Core queue가 record를
binding에 넘기면 그 record의 Core HWM charge는 끝난다. Framework는 retained-credit lease를
요청하거나 Core byte charge를 handler·reply lifetime까지 연장하지 않는다.

Receive 뒤 payload storage는 [Payload 소유권과 복사](50-internal-message-ownership.ko.md)의 일반
message ownership을 따른다. Storage를 복사하거나 이동하고 해제하는 일은 payload lifetime 관리이며
HWM credit, 두 번째 job permit 또는 별도 byte-pressure authority가 아니다.

Framework pressure count는 다음 값이다.

```text
application job permits in use
  = reserved supply permits
  + queued application jobs
```

Capacity waiter는 아직 permit을 받지 않았으므로 포함하지 않는다. Reservation이 queued job으로
바뀌어도 합계는 변하지 않고 permit을 반환할 때만 감소한다. 한 record가 1:N callback을 만들면 실제
callback turn마다 permit 하나를 사용한다.

## 3. 설정과 profile 경계

Core와 Framework profile은 같은 label을 사용할 수 있지만 서로 다른 public type과 계산이다.

| 설정군 | 소유자 | 기본 profile | Manual override |
|---|---|---|---|
| `CoreHwmMemoryLimitBytes`, `CoreHwmBudgetBytes`, `CoreHwmProfile` | Core | `Balanced` | Core memory 또는 budget byte |
| `ApplicationJobQueueProfile`, `MaxQueuedApplicationJobs` | Framework host | `Balanced` | Host의 정확한 job permit 상한 |

Framework는 startup에서 Core 설정값을 binding context option으로 전달할 수 있지만 profile 비율을
계산하거나 budget을 connection 수로 나누지 않는다. Core snapshot을 Framework status에 투영하는 것도
읽기 전용 관측이며 Framework pressure 계산의 입력이 아니다.

Runtime에서 Framework job pressure가 Core에 주는 feedback은 지원 socket에 적용하는
`RUNNING`·`PAUSED` receive-flow 절대 상태 하나뿐이다. Framework는 Core HWM 설정이나 queued-byte
counter를 상태 전이에 맞춰 변경하지 않는다.

## 4. Ordinary ingress permit 순서

Ordinary ingress는 다음 순서를 지킨다.

1. 가장 오래 기다린 live source 순서로 host-shared permit을 기다린다.
2. Permit을 얻은 뒤에만 Core/binding에서 record를 receive·claim한다.
3. Application record는 permit을 owner mailbox나 serial queue의 handler turn으로 이전한다.
4. Control 또는 malformed ordinary record는 유한한 내부 처리를 마친 뒤 reservation을 반환한다.
5. 공통 invocation boundary는 callback의 실제 첫 instruction 직전에 permit을 반환한다.

Handler가 시작한 뒤의 `await`, coroutine suspension, continuation과 reply 대기는 같은 permit을 다시
얻지 않는다. Relocation처럼 아직 runnable하지 않은 durable backlog는 해당 relocation spec이 정한
유한한 payload owner로 handoff한 뒤 initial reservation을 반환하고, runnable callback turn마다 새
permit을 얻는다.

Pre-receive에 terminal reply 또는 error reply completion으로 식별되는 supply만 ordinary ingress
permit을 우회한다. Ordinary connection에서 receive한 뒤 completion으로 분류해 우회를 소급 적용하지
않는다. 이 분리는 ordinary queue가 포화돼도 이미 시작한 operation의 terminal completion이 진행되게
한다.

Framework heartbeat, topology, relocation과 service-wire `SendReady` kind `12`는 이 completion
supply가 아니다. 이 control record는 application data line에 남아 기존 FIFO와 liveness 계약을
따르며, Core completion 연결이나 별도 Framework control queue로 옮기지 않는다.

다음 방식은 허용하지 않는다.

- permit 없이 먼저 receive한 뒤 별도 counter만 증가시키기
- retained-credit lease나 Framework byte-HWM으로 Core HWM을 다시 구현하기
- 포화를 reject, drop, fixed-delay polling 또는 busy spin으로 바꾸기
- spec이 소유하지 않는 unbounded 또는 hidden side backlog에 record 보관하기

## 5. Pressure 상태와 Core 연결

Effective maximum을 `M`, configured pause·resume percent를 `P`와 `R`이라고 하면 startup에서 다음
경계를 계산한다.

```text
pause permit count  = ceil(M * P / 100)
resume permit count = floor(M * R / 100)
```

`P`는 `1..100`이고 기본값은 `80`, `R`은 `0..99`이고 기본값은 `60`이다. `R < P`여야 한다.
`running`에서는 permits in use가 pause count 이상이면 `paused`로 전이하고, `paused`에서는 resume
count 이하이면 `running`으로 전이한다. 두 경계 사이에서는 현재 상태를 유지한다.

Framework는 전이한 절대 상태를 RouteMesh와 ClientServer가 사용하는 paired DEALER/ROUTER socket에만
적용한다. PUB/SUB, Classic fanout과 STREAM은 이 연동 범위가 아니며 기존 Core byte HWM과 각 구조적
queue 상한을 유지한다.

`PAUSED`는 Core HWM 값을 바꾸지 않는다. Core는 remote-pause blocker와 local byte-HWM blocker를
독립적으로 합성한다. `RUNNING`은 remote-pause 원인만 제거하므로 local HWM이 계속 full이면 send는
계속 대기한다. Pressure 상태 자체는 route ready나 transport liveness를 바꾸지 않는다.

## 6. Socket lifecycle과 단일 제어 지점

Host queue owner는 permit count 변경과 같은 synchronization 경계에서 pressure 상태를 계산한다.
상태가 바뀌면 지원 socket snapshot에 새 절대 상태를 적용한다. 같은 상태를 반복 적용하지 않고,
이전 transition sequence가 최신 상태를 덮어쓰지 못하게 한다.

새 socket은 현재 host pressure 상태를 적용한 뒤 receive 대상 registry에 게시한다. Close는 먼저
registry에서 socket을 제거한 뒤 진행한다. Binding 호출은 queue, registry와 user callback lock 밖에서
수행한다. Close와 경쟁한 lifecycle 결과 외의 설정 실패는 진단과 metric에 기록한다.

이 receive-flow state API가 Framework pressure와 Core send flow 사이의 유일한 runtime 제어 지점이다.
Framework는 raw flow frame을 만들거나 Core control lane을 범용 Framework channel로 사용하지 않는다.

## 7. Send completion과의 합성

Core와 binding은 HWM 대기, 내부 재시도와 operation별 completion을 소유한다. Framework는 exact target을
선택한 뒤 binding operation 하나만 시작한다. Operation이 시작되면 PAUSE나 HWM을 이유로 target을 다시
고르거나 같은 payload로 두 번째 operation을 만들지 않는다.

Framework는 제거된 `send_ready` callback·event, readiness waiter 또는 retry adapter를 두지 않는다.
Deadline, cancellation, detach와 shutdown은 기존 operation state machine의 첫 terminal 규칙을 따른다.
Framework service-wire의 `SendReady` kind `12`는 Framework service control record이므로 제거된 binding
callback과 다른 계약이다.

## 8. 큰 payload와 운영값

Application Job Queue는 job count를 제한하지 payload byte를 가중하지 않는다. 빈 payload와 큰 payload는
각각 job 하나다. 따라서 Framework queue 상한은 process memory의 byte hard cap이 아니다.

큰 payload를 오래 보유하는 workload는 production과 같은 payload 분포, permits in use, process memory,
throughput과 latency를 함께 측정해 `MaxQueuedApplicationJobs`를 낮춘다. 단일 message 크기는
`MaxMessageSize`로 별도 제한한다. 이 문제를 해결하려고 Core profile을 Framework profile에 연결하거나
retained-credit lease를 복구하지 않는다.

Core HWM은 Core queue memory의 마지막 안전장치로 계속 동작한다. Framework가 permit 때문에 ordinary
receive를 멈추면 local Core receive queue에 byte가 쌓이고, finite Core HWM과 TCP backpressure가 sender의
진행을 제한한다.

## 9. Contract test 요구사항

각 Framework runtime은 최소한 다음을 검증한다.

- Core profile과 Application Job Queue profile을 서로 다르게 설정할 수 있고 둘의 기본값은 각각
  `Balanced`다.
- Permit이 없으면 다음 ordinary record를 먼저 receive하지 않는다.
- Reservation, queued job과 callback 첫 instruction의 permit count가 같은 규칙을 따른다.
- 80% pause, 60% resume, 경계 사이 hysteresis와 같은 상태 중복 억제가 정확하다.
- 새 socket 동기화, close 경쟁과 stale transition이 최신 절대 상태를 깨지 않는다.
- 지원 paired socket에만 receive-flow state를 적용한다.
- Completion supply는 ordinary permit 포화와 독립적으로 진행한다.
- Framework가 retained receive, `send_ready` waiter 또는 별도 send retry를 사용하지 않는다.

설정의 exact 값은 [Framework API](06-framework-api.ko.md), 상태와 metric은
[Runtime 상태](24-runtime-monitoring.ko.md)와 [Runtime metric](25-runtime-metrics.ko.md), permit 구현은
[수신과 dispatch loop](46-internal-dispatch-loop.ko.md)가 정의한다.
