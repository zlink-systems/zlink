# Bindings Auto HWM 메모리 예산 적용 계획

## 문서 상태

- 상태: 구현 전 계획
- 대상: C++, .NET, Java, Node.js, Python, Go, Rust bindings
- 공통 정책: [Auto HWM 메모리 예산 및 CCU 재계산 계획](auto-hwm-memory-budget-and-ccu-recalculation-plan.ko.md)
- Core 구현: [Core 적용 계획](auto-hwm-memory-budget-core-plan.ko.md)
- Framework 연동: [Framework 적용 계획](auto-hwm-memory-budget-framework-plan.ko.md)

## 1. 목적

Bindings는 Core의 메모리 예산 옵션과 snapshot을 각 언어의 자연스러운 API로 노출한다.
프로필 비율, CCU, queue 수, HWM은 bindings에서 계산하지 않는다.

managed runtime을 사용하는 언어는 사용자가 메모리 한도를 지정하지 않았을 때 VM 또는 heap 한도를 Core에 힌트로 전달할 수 있다.
Framework가 명시적인 값을 전달하면 binding의 자동 감지는 실행하지 않거나 그 값보다 낮은 우선순위로 취급한다.

## 2. 현재 구현

현재 bindings는 Core context 옵션을 변환하여 전달하고 monitor 구조체를 언어별 객체로 변환한다.
Auto HWM profile과 message unit은 노출하지만 context 전체 Core 예산과 aggregate snapshot API는 없다.

근거:

- [Bindings 공통 사양](../../bindings/doc/spec/README.ko.md)
- [Core context 공개 API](../../core/include/zlink/core/api.h)
- [Core monitor 공개 API](../../core/include/zlink/eventing/api.h)

현재 계산은 Core에서 수행되므로 binding별로 같은 profile을 선택해도 계산 엔진은 하나다.
이 원칙은 새 정책에서도 유지한다.

현재 routed 비동기 전송의 최초 Core 수용 방식은 언어마다 다르다. .NET과 Node.js의 일부 request 경로는
비동기 반환 객체가 있어도 그 객체를 반환하기 전에 native blocking submit을 실행한다. Java Framework는
blocking을 피하지만 10ms 주기 재시도를 Framework에 구현한다. 이 차이는 [§9.3](#93-현재-언어별-구현-gap)에
정리하며, 목표 구현은 모든 binding에서 같은 비동기 수용 결과를 보장한다.

비동기 경로 확인 근거:

- [.NET Framework raw request](../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkRawRouterServicePort.cs)
- [.NET binding Router submit](../../bindings/dotnet/src/Zlink/Runtime/Sockets/RouterSocket.cs)
- [Java Framework raw service port](../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java)
- [Java Framework request 재시도](../../framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRequestSubmitter.java)
- [Node.js Framework raw binding port](../../framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts)
- [Node.js binding request executor](../../bindings/node/src/zlink/runtime/messaging/request_executor.ts)
- [Node.js native request submit](../../bindings/node/native/src/addon_core.cc)

## 3. 책임 경계

| 계층 | 수행할 일 | 수행하지 않을 일 |
|---|---|---|
| Binding | 옵션 전달, 타입 변환, runtime memory hint 감지, snapshot 변환 | 프로필 비율 계산, CCU 계산, per-queue HWM 계산 |
| Core | 입력 우선순위, 프로필 계산, 물리 queue 분배와 connection별 HWM 적용 | 언어별 VM API 직접 호출 |
| Framework | 애플리케이션 설정을 binding context에 전달 | binding과 별도의 Core budget 생성 |

Framework에서 binding으로 전달된 값과 binding 사용자가 직접 설정한 값은 같은 Core context 옵션으로 들어간다.
Framework용 Core 예산과 binding용 Core 예산을 따로 만들지 않는다.

## 4. 공통 API 모델

모든 binding은 다음 개념을 동일하게 노출한다.

| 개념 | 타입 | 0의 의미 |
|---|---|---|
| memory limit bytes | unsigned 64-bit byte | 명시 값 없음 |
| runtime memory limit bytes | unsigned 64-bit byte | 감지 값 없음 |
| manual Core budget bytes | unsigned 64-bit byte | 수동 예산 없음 |
| monitor HWM bytes | unsigned 64-bit byte | Core monitor 기본값 사용 |
| profile | enum | 해당 없음 |
| budget snapshot | immutable value/object | 해당 없음 |
| metrics reset | context operation | 해당 없음 |

manual Core budget은 프로필 계산 결과를 직접 고정하는 값이다.
socket SNDHWM 또는 RCVHWM과 이름과 API 위치를 분리해야 한다.

## 5. 런타임 메모리 힌트

### 5.1 선택 규칙

binding은 사용자가 memory limit 또는 manual Core budget을 명시하지 않았을 때만 runtime hint 자동 감지를 시도한다.
감지 결과를 Core의 runtime memory limit 옵션으로 전달하고 비율 계산은 Core에 맡긴다.

각 언어의 후보는 다음과 같다.

| Binding | runtime hint 후보 | 감지 실패 시 |
|---|---|---|
| C++ | 없음 | Core의 container, process, OS 감지 |
| .NET | GC가 보고하는 사용 가능 메모리 한도 | Core 감지 |
| Java | JVM 최대 heap | Core 감지 |
| Node.js | V8 heap size limit | Core 감지 |
| Python | 별도 VM 한도가 명확할 때만 사용 | Core 감지 |
| Go | 유한한 runtime memory limit이 설정된 경우 | Core 감지 |
| Rust | 없음 | Core 감지 |

Runtime heap 한도와 Core가 감지한 finite container/process hard limit이 모두 있으면 Core는 두 값의
최솟값을 effective memory로 사용한다. Binding은 runtime 원본 값을 전달할 뿐 hard limit과 직접
결합하거나 profile 비율을 적용하지 않는다.

### 5.2 명시 설정과 Framework 호출

다음 우선순위를 지킨다.

1. Framework 또는 binding 사용자가 지정한 manual Core budget
2. Framework 또는 binding 사용자가 지정한 memory limit
3. binding이 감지한 runtime memory limit
4. Core fallback

Explicit memory limit 또는 manual Core budget이 Core가 감지한 finite hard limit보다 크면 Core의
`EINVAL`을 그대로 반환한다. Binding은 값을 조용히 clamp하지 않는다.

Framework는 binding의 동일한 공개 setter를 호출한다.
내부 전용 우회 API를 만들지 않는다.

## 6. 언어별 표면

다음 이름은 구현 전 contract pseudocode다. 실제 signature는 각 binding의 기존 Context/Options 객체와
naming convention에 맞춰 확정하되 의미와 byte 단위는 바꾸지 않는다.

| Binding | 설정 예시 | 조회 예시 |
|---|---|---|
| C++ | CoreHwmMemoryLimitBytes, CoreHwmBudgetBytes, CoreHwmProfile | Core HWM snapshot, reset metrics |
| .NET | CoreHwmMemoryLimitBytes, CoreHwmBudgetBytes, CoreHwmProfile | GetCoreHwmBudgetSnapshot |
| Java | coreHwmMemoryLimitBytes, coreHwmBudgetBytes, coreHwmProfile | coreHwmBudgetSnapshot |
| Node.js | coreHwmMemoryLimitBytes, coreHwmBudgetBytes, coreHwmProfile | getCoreHwmBudgetSnapshot |
| Python | core_hwm_memory_limit_bytes, core_hwm_budget_bytes, core_hwm_profile | core_hwm_budget_snapshot |
| Go | CoreHwmMemoryLimitBytes, CoreHwmBudgetBytes, CoreHwmProfile | CoreHwmBudgetSnapshot |
| Rust | core_hwm_memory_limit_bytes, core_hwm_budget_bytes, core_hwm_profile | core_hwm_budget_snapshot |

설정은 context가 socket 또는 runtime owner를 만들기 전에 완료하도록 권장한다. 기존 slot policy mode와
ApplicationHwm 호환 이름은 노출하지 않는다.
실행 중 변경을 지원하면 Core의 budgetGeneration과 재계산 결과가 반환될 때까지의 비동기 의미를 문서화한다.

각 binding의 monitor open option은 언어 naming convention에 맞는 `monitorHwmBytes`를 제공한다. 값은
Core로 변환 없이 전달하며 event count를 곱하거나 나누지 않는다. 0은 Core 기본값 선택이다.

## 7. 숫자와 ABI 변환

Core byte 필드는 uint64 범위를 사용한다.

- C++, Rust와 .NET은 기존 byte option 관례에 맞춰 uint64, u64와 ulong으로 노출한다.
- Java는 양수 long 범위만 지원하고 Long.MAX_VALUE를 넘는 Core 값은 overflow 오류로 처리한다.
- Node.js는 안전한 정수 범위를 넘을 수 있으므로 BigInt를 사용한다.
- Python은 정수 범위를 허용하되 Core uint64 범위를 검사한다.
- Go는 uint64를 사용한다.
- 모든 binding은 overflow, 음수, NaN, 소수 입력을 호출 전에 거부한다.

snapshot에는 version과 struct size를 보존한다.
구버전 Core가 새 API를 지원하지 않으면 0값을 조용히 반환하지 말고 명시적인 unsupported 오류를 제공한다.
Binding은 caller가 abi_version과 struct_size를 초기화하는 Core 규칙을 내부에서 수행하고, EINVAL,
ENOTSUP과 ETERM을 언어별 기존 error mapping으로 변환한다.

## 8. snapshot 매핑

모든 binding은 Core snapshot 필드를 의미 변경 없이 노출한다.
최소 필드는 다음과 같다.

- configured, runtime, resolved memory limit
- configured, effective Core budget
- total planned, total applied HWM bytes
- manual reserved HWM bytes
- Core queue, application-held, current와 peak accounted bytes
- completion current·peak·pending message와 total messaging accounted bytes
- monitor queue applied HWM·accounted byte와 total instance applied/accounted byte
- oversize admission count와 largest oversize message bytes
- HWM-controlled active directional/send/receive queue count와 별도 completion directional queue count
- unlimited manual queue count와 aggregate validity
- aggregate overflow와 provisional accounted bytes
- budget insufficient 상태
- blocked ratio ppm
- budget generation과 measurement epoch

totalPlannedHwmBytes와 totalAppliedHwmBytes는 completion을 제외한 auto와 manual physical queue를 한 번씩 포함한다.
totalAppliedHwmBytes는 예약된 실제 메모리가 아니라 현재 pipe HWM 상한의 합이다.
currentAccountedBytes는 Core queue와 Framework가 retained lease로 보유한 applicationAccountedBytes의
합이며 completion byte를 포함하지 않는다. `totalMessagingAccountedBytes = currentAccountedBytes +
completionCurrentAccountedBytes`이고 completion 필드는 진단 전용이라 admission과 Core budget을 바꾸지
않는다. peakAccountedBytes와 completionPeakAccountedBytes는 각각 같은 범위의 peak다.
activeSendQueueCount와 activeReceiveQueueCount는 completion을 제외한 HWM-controlled application queue만
세며 같은 physical queue를 각 관점에서 포함할 수 있으므로
둘을 더해 activeDirectionalQueueCount로 사용하지 않는다.
activeCompletionDirectionalQueueCount는 completion queue만 별도로 센다.
각 binding 문서와 타입 주석에서 이 차이를 같은 문장으로 설명한다.

## 9. 기존 socket HWM과의 관계

기존 SNDHWM과 RCVHWM API는 유지한다.
이 값은 socket 방향별 manual per-pipe 상한이며 context manual Core budget과 다르다.

binding API 이름에서 다음 구분이 드러나야 한다.

- CoreHwmBudgetBytes: context 전체 Core-managed messaging 예산
- SendHwmBytes, ReceiveHwmBytes: socket 방향별 manual HWM
- CoreHwmBudgetSnapshot: context 전체 현황

manual socket HWM 0의 unlimited 의미도 유지한다. `CoreHwmBudgetBytes`는 auto pipe HWM의 계산 기준이며
manual pipe나 context 전체 실제 사용량에 hard cap을 추가하지 않는다.

DEALER·ROUTER completion lane에는 public SendHwmBytes·ReceiveHwmBytes를 적용하지 않는다. Binding은
reply와 error reply를 Core의 HWM 없는 completion progress lane으로 그대로 전달하며 별도 completion
HWM setter를 추가하지 않는다. Router completion-control submit·handler API는 모든 binding에서 alias나
deprecated surface 없이 제거한다. Framework control은 기존 Router routed send/receive binding API를
사용한다.

Binding은 큰 message를 이유로 별도 사전 크기 검사를 추가하지 않는다. Core가 빈 A pipe에 HWM보다 큰
complete message 한 건을 수용하면 A의 다음 send만 HWM에 막힌다. B·C·D connection의 send는 각각의
pipe 상태에 따라 계속된다. Binding은 A의 초과분을 다른 connection의 pending 상태로 변환하거나 context
전체 backpressure로 확대하지 않는다. `MAXMSGSIZE`는 사용자가 별도로 설정했을 때만 기존 Core 오류를
그대로 전달한다.

### 9.1 Monitor HWM byte API와 perf 옵션

모든 binding의 monitor open API는 HWM을 byte로만 받는다. Perf CLI와 환경 변수도
`--monitor-hwm-bytes`, `PERF_MONITOR_HWM_BYTES`, `PERF_MULTI_MONITOR_HWM_BYTES`만 사용한다. 기존
`--monitor-hwm`, `PERF_MONITOR_HWM`, `PERF_MULTI_MONITOR_HWM`은 alias나 자동 변환 없이 제거한다.

Snapshot은 send/receive pending message count와 pending byte를 모두 노출한다. Count는 표시용이며 HWM
설정이나 admission에 사용하지 않는다. Legacy `autoHwmSocketMessageSlots`, message-unit, size-cap과
connection-bucket 진단 property는 각 binding public type에서 제거한다.

### 9.2 비동기 최초 수용과 target 격리

모든 binding은 routed send와 routed request에 두 종류의 호출을 구분해 제공한다.

- Nonblocking submit은 Core에 한 번 제출하고 `OK`, `BACKPRESSURED`, `NOT_FOUND`, `DISCONNECTED`,
  `TERMINATED`를 즉시 반환한다.
- 비동기 전송은 target pipe가 HWM 때문에 수용하지 못하면 해당 operation만 기다렸다가 전송 가능 상태에서
  다시 진행한다. Send는 Core 수용 시 완료하고 request는 수용 뒤 reply lifecycle까지 계속 기다린다.

비동기 전송은 HWM 대기 중 호출 언어의 event loop 또는 runtime worker thread를 점유하지 않는다. 같은
socket의 공용 submit lock을 잡은 채 기다리지도 않는다. A RID가 막혀도 B·C·D RID의 호출은 Core에
독립적으로 도달하고 진행할 수 있어야 한다. `Task`, `CompletableFuture` 또는 `Promise`를 반환하기 전에
native blocking submit을 실행하는 방식은 비동기 계약을 만족하지 않는다.

Binding은 Core의 nonblocking 결과와 대상 pipe의 전송 가능 상태 변화를 이용해 operation을 재개한다.
수용 실패 뒤 waiter를 등록하는 사이에 wake를 잃지 않도록 waiter를 먼저 등록하고 수용을 시도하거나,
등록 뒤 같은 pipe 상태를 다시 검사해야 한다. A가 막힌 동안 B가 writable해서 socket-wide send-ready가
먼저 발생하거나 소비되어도 A operation은 영구 대기해서는 안 된다.

이 대기 상태는 binding 내부 구현이며 Framework에 RID map, retry deque, ready ring이나 주기적 polling을
요구하지 않는다. 별도 public retry queue capacity와 queue-full 오류도 추가하지 않는다. 대상 pipe의 credit
회복은 해당 operation을 재개하고, pipe detach, socket close, context 종료, timeout과 cancellation은 이를
정확히 한 번 terminal 완료한다. 최초 호출의 deadline은 대기 중 연장하지 않는다.

Multipart가 아직 수용되지 않았으면 binding은 complete record와 caller-visible operation을 계속 소유한다.
Core 수용 성공 뒤에는 Core가 payload와 request lifecycle을 소유한다. Request가 wire에 보이기 전에 reply
correlation이 준비되어야 하며 빠른 reply가 등록보다 먼저 도착해 유실되어서는 안 된다. 이 순서는 Core와
binding 내부 계약이고 Framework가 callback 등록 상태를 별도로 관리하지 않는다.

### 9.3 현재 언어별 구현 gap

| Binding | 현재 동작 | 목표와의 차이 |
|---|---|---|
| C++ | One-way send는 nonblocking이지만 routed request는 공유 socket mutex 안에서 blocking submit을 호출한다. | Request 수용 대기가 다른 RID 호출을 지연시킬 수 있다. |
| .NET | `RequestAsyncCore`가 `Task`를 반환하기 전에 `SubmitGate` 안에서 flags 0 native request를 호출한다. 일부 다른 경로만 `DONTWAIT` submitter를 사용한다. | Native 수용 대기가 managed thread와 같은 socket submit을 막을 수 있다. |
| Java | Framework send/request는 `DONT_WAIT`이고 backpressure면 Framework scheduler가 10ms 주기로 재시도한다. | Thread block은 피하지만 비동기 수용 책임과 polling이 Framework에 중복 구현되어 있다. |
| Node.js | Raw request의 Promise executor가 flags `None` native request를 JS event loop에서 동기 호출한다. 일부 channel 경로만 `DONT_WAIT` submitter를 사용한다. | Native 수용 대기가 event loop 전체를 멈출 수 있다. |
| Python·Go·Rust | 이 계획에서 전체 routed async call path 감사를 아직 완료하지 않았다. | 구현 전에 반환 객체 생성 전 native blocking submit, socket-wide lock 대기와 Framework polling 여부를 확인해야 한다. |

구현은 위 차이를 제거해야 한다. 특정 언어가 이미 nonblocking submitter를 갖고 있더라도 Framework 소유
queue를 그대로 표준으로 삼지 않고 binding 비동기 전송 계약으로 수렴한다.

## 10. Framework retained-credit bridge

일반 binding recv는 message를 application에 반환할 때 Core queue credit을 즉시 반환한다. Framework
backend만 retained-credit recv를 사용해 같은 credit을 opaque lease로 넘겨받는다.

- Lease wrapper는 move-only 또는 언어별 단일 소유 handle이다.
- Lease는 Core의 origin queue ID, generation과 accounted byte를 보존하지만 public setter로 노출하지 않는다.
- Framework queue, executor와 handler로 이동해도 같은 lease를 유지한다.
- Job 완료, drop, cancel과 오류에서 정확히 한 번 release한다.
- .NET SafeHandle, Java AutoCloseable, Node.js native finalizer, Python context manager, Go Close와 Rust Drop은
  누락 방지용 fallback이며 정상 job 완료 경로가 명시적으로 먼저 release한다.
- Context shutdown은 신규 lease 생성을 막고 outstanding lease 수를 진단한다.
- Connection detach는 lease를 무효화하지 않으며 Core retired queue에 연결된 상태로 release할 수 있어야 한다.

Lease는 별도의 Framework budget이 아니며 원래 Core pipe credit의 owner만 바꾼다. Binding은
ApplicationHwmBytes/Profile 설정이나 별도 percent 계산 API를 제공하지 않는다.

Retained receive 성공은 physical dequeue와 lease 생성이 하나의 원자적 결과여야 한다. Lease wrapper
생성에 실패하면 Core receive를 rollback하거나 lease를 즉시 반환한 뒤 오류를 전달한다. Message만
Framework에 전달하고 lease를 잃는 부분 성공은 허용하지 않는다.

큰 message용 allowance, context owner 또는 lease-to-send exchange API는 노출하지 않는다. 각 pipe의
empty-pipe complete-message 한 건 예외는 Core가 독립적으로 처리한다.

CoreHwmBudgetBytes의 범위는 Core context 하나다. Binding은 여러 context에 같은 값을 자동 복제하지
않으며 여러 context의 예산을 하나처럼 합산하지 않는다.

## 11. 구현 순서

1. Core C header와 ABI 버전 반영
2. 각 binding의 CoreHwm context setter/getter 추가
3. Snapshot 구조체와 reset operation 매핑
4. Retained-credit recv와 lease handle mapping 추가
5. .NET, Java, Node.js, Go runtime hint adapter 추가
6. 숫자 범위와 unsupported Core 오류 처리
7. ApplicationHwm 이름을 alias 없이 제거
8. Framework control이 사용하는 기존 routed send/receive 호출 경로 통합
9. Framework 이관 완료 뒤 Router completion-control API·callback wrapper와 native symbol mapping 제거
10. Monitor HWM byte open option과 pending byte·instance aggregate 매핑 추가
11. 모든 perf launcher의 legacy monitor count 옵션과 slot·bucket snapshot property 제거
12. Routed send/request의 nonblocking 결과와 최초 수용까지 포함하는 비동기 API를 모든 binding에 통일
13. .NET SubmitGate 대기, Java Framework 10ms polling과 Node.js event-loop native blocking gap 제거

## 12. 검증 항목

- 모든 binding에서 동일 입력이 같은 effective Core budget을 생성
- manual Core budget에는 프로필 비율이 다시 적용되지 않음
- explicit memory limit이 runtime hint보다 우선함
- runtime hint와 finite Core hard limit의 최솟값 사용
- hard limit을 넘는 explicit/manual 입력의 EINVAL 전달
- Framework 호출과 사용자의 직접 binding 호출이 같은 Core 옵션을 설정
- ApplicationHwm 이름이 compile/configuration 단계에서 거절됨
- Node.js BigInt와 각 언어의 uint64 경계 처리
- Oversize allowance나 budget 기반 MAXMSGSIZE API가 노출되지 않음
- A의 oversize message 뒤에도 B·C·D send 결과를 Core에서 받은 그대로 전달
- Completion snapshot 4개 필드와 activeCompletionDirectionalQueueCount를 누락 없이 매핑
- Completion이 totalPlanned/Applied HWM과 active application queue 분모에서 제외되는지 확인
- Metrics reset 뒤 current·pending은 유지되고 peak는 current로 재기준화되며 epoch counter만 초기화됨
- 모든 binding에서 Router completion-control submit·handler와 native symbol mapping이 제거됨
- Framework control이 기존 Router routed send/receive API를 사용함
- 모든 binding에서 동일 monitorHwmBytes가 Core에 변환 없이 전달됨
- 0은 Core 기본값을 선택하고 양수 최대 범위·overflow가 언어별 규칙대로 처리됨
- Pending message count와 pending byte가 단위 변환 없이 각각 노출됨
- Legacy monitor-hwm CLI·환경 변수와 slot·bucket 진단 property가 제거됨
- Routed send/request의 nonblocking API가 BACKPRESSURED를 즉시 손실 없이 반환함
- 비동기 API는 backpressure 중 caller runtime thread와 socket-wide submit lock을 점유하지 않음
- A RID의 비동기 수용 대기가 B·C·D RID의 submit과 진행을 막지 않음
- A가 막히고 B가 writable인 상태에서 socket-wide ready가 먼저 발생해도 A operation의 wake가 유실되지 않음
- Waiter 등록과 Core 상태 재검사 사이의 경쟁을 반복 시험해 영구 대기가 없음
- Pipe detach, socket close, context 종료, timeout과 cancellation이 대기 operation을 정확히 한 번 완료함
- Backpressured multipart를 binding operation이 보존하고 수용 성공 또는 terminal 결과에서 한 번만 정리함
- 빠른 reply가 request correlation 준비 전 도착해 유실되지 않음
- .NET `Task` 반환 전 native blocking submit, Java Framework 10ms submit polling과 Node.js Promise executor의
  native blocking submit이 제거됨
- .NET ulong과 Java 양수 long 범위 처리
- snapshot 필드 누락, 단위 변환, signed overflow 방지
- metrics reset 후 measurementEpoch 증가
- socket manual HWM과 context Core budget API 혼동 방지
- 구버전 Core 사용 시 명시적인 unsupported 처리
- Retained lease의 move, 완료, drop, cancel, 예외와 shutdown별 단일 release
- Lease wrapper 생성 실패의 receive rollback과 credit 누수 방지
- Connection detach 뒤 retired origin lease release
- Oversize lease도 일반 lease와 같은 origin queue 소유권을 보존
- 여러 context에 같은 budget이 암묵적으로 중복 적용되지 않음
