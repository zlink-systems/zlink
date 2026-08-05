<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: 등록·codec](config-4-registration-codec.ko.md) | [다음: Store 장애·복구](config-6-store-failure-recovery.ko.md)
<!-- framework-adapter-nav:end -->

# Config 5 — Process restart와 lifecycle 복구

Service process는 정상 종료, crash, replacement와 network 단절을 겪는다. Framework는 current ready target을
public status에 반영하고, 이미 끝난 operation을 새 target에 자동 재제출하지 않으며, Host maintenance에서
accepted work와 신규 admission을 구분해야 한다.

이 config는 runner가 process와 network를 외부에서 조작하고 public request·send, Host operation, status와
application evidence로 결과를 확인한다. Raw protocol command, private connection ID, Store record와 internal
relocation queue는 사용하지 않는다.

## 1. 확인 범위

- Server restart, endpoint replacement, reconnect burst와 rolling update
- Cancellation, in-flight crash, graceful shutdown과 runtime weight 변경
- Repeated lifecycle, Store 독립, fanout load와 observer failure
- Orderly disconnect, half-open connection과 terminal-once completion
- Relocation preflight, Session binding fence, temporary handoff ordering과 abort 복원

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Location Store | 1 | Automatic provider discovery와 object 위치를 제공한다. |
| Relocation Store | 1 | `PreserveStateWith` Actor·User Spot relocation state를 보존한다. |
| Channel provider | 2~3 | Weighted Channel handler와 runtime status를 제공한다. Version과 endpoint가 다른 replacement를 시작할 수 있다. |
| Object Server | 2~3 | Actor, Entry·User·Instance Spot factory와 relocation adapter를 제공한다. |
| Session gateway | 1 | Stream Session과 Actor binding을 제공한다. |
| E2E client | scenario별 | Public application endpoint와 Stream endpoint로 지속 traffic을 보낸다. |

Runner는 `SIGTERM`, `SIGKILL`, process restart와 network pause·blackhole을 제공한다. 각 scenario는 fresh
process와 Store namespace를 사용한다. Application handler·adapter gate는 public evidence와 release endpoint를
제공하며 Framework state를 직접 변경하지 않는다.

## 3. 공통 실행과 판정 방법

Recovery는 public status의 current ready targets와 follow-up request 성공을 함께 확인한다. Request마다 고유
operation ID를 사용하고 reply, error, timeout 또는 cancellation 중 terminal 하나만 허용한다. Send terminal은
remote handler 완료로 간주하지 않는다.

Readiness와 lease 경계는 configured timeout에서 계산한 bounded polling을 사용한다. Provider가 다시
선택되는지를 확률에 맡기지 않는다. 특정 replacement를 확인해야 할 때 다른 provider의 weight를 0으로 바꾼
뒤 public status 전파를 확인하고 directed 검증 구간을 실행한다.

## 4. Scenario

### Track A — Process restart와 replacement를 처리

#### RL-A1 같은 endpoint에서 server를 재시작한다

우선순위: `P1`

Server가 정상 종료한 뒤 같은 endpoint에서 새 lifecycle로 시작되어도 consumer를 재시작할 필요가 없다.

**검증 질문:** Down 구간 request는 bounded failure이고 새 server ready 직후 request는 성공하는가.

- 시작 조건: 유일한 provider가 ready이고 baseline request가 성공했다.
- 절차: Provider에 public Shutdown을 호출하고 current target이 없음을 확인한다. Down 구간 request를 한 번
  보내고 같은 endpoint에서 replacement를 시작한다. Ready 직후 follow-up request를 보낸다.
- 검증: Down request는 `NotFound` 또는 정식 route terminal로 한 번 끝나며 자동 재제출되지 않는다. Follow-up
  request는 replacement handler에서 한 번 처리되고 consumer process는 유지된다.
- 세부 동작: [Transport liveness §6](../spec/29-transport-liveness.ko.md)을
  검증한다.

#### RL-A2 다른 endpoint의 replacement로 전환한다

우선순위: `P2`

Pod replacement는 endpoint와 Node RID가 모두 바뀔 수 있다. Consumer는 current automatic descriptor를 따라가야
한다.

**검증 질문:** Old provider crash 뒤 새 endpoint replacement만 follow-up requests를 처리하는가.

- 시작 조건: Old provider가 유일한 target이고 slow request handler에 들어간 상태다.
- 절차: Old provider를 강제 종료하고 pending request terminal을 수집한다. 다른 endpoint에서 replacement를
  시작하여 ready를 확인한 뒤 requests 20개를 보낸다.
- 검증: Pending request는 `Unavailable` 또는 `DeadlineExceeded` 중 하나로 한 번 끝난다. Follow-up 20개는
  모두 replacement가 처리하고 old endpoint로 자동 재제출되지 않는다.
- 세부 동작: [Failover policy §3](../spec/31-failure-failover-policy.ko.md)을 검증한다.

#### RL-A3 많은 clients가 server restart 뒤 reconnect한다

우선순위: `P1`

동시에 연결된 clients가 많아도 server restart 뒤 Framework connector의 reconnect로 복구되어야 한다.

**검증 질문:** Client 100개가 replacement ready 뒤 별도 reconnect loop 없이 request를 한 번씩
완료하는가.

- 시작 조건: 100 clients가 각각 baseline request를 성공했다.
- 절차: Server를 정상 종료하고 replacement를 시작한다. 각 connector의 public ready 상태를 bounded
  polling한 뒤 고유 request를 한 번씩 보낸다.
- 검증: 모든 connector가 common reconnect timeout 안에 ready이고 100 replies가 operation ID별로 한 번씩
  도착한다. Application이 reconnect를 반복 호출하지 않는다.
- 세부 동작: [Transport liveness §6](../spec/29-transport-liveness.ko.md)을
  검증한다.

#### RL-A4 Rolling과 blue-green update 중 serving target을 유지한다

우선순위: `P2`

새 version target이 ready인 것을 확인한 뒤 old provider를 하나씩 제외해야 전환 중 target 수가 0이 되지
않는다.

**검증 질문:** 지속 requests 중 old set을 new set으로 교체해도 terminal 누락 없이 완료 뒤 new version만
처리하는가.

- 시작 조건: Version N providers가 ready이고 continuous request workload가 실행 중이다.
- 절차: N+1 providers를 시작하고 public status와 직접 request로 readiness를 확인한다. Rolling variant는
  old provider를 한 대씩 Relocate·Shutdown하고, blue-green variant는 green set 전체 ready 뒤 blue set을
  순서대로 종료한다.
- 검증: 각 request는 terminal 하나를 받고 serving target count가 0이 되지 않는다. 완료 뒤 신규 requests는
  N+1 handler evidence에만 기록된다. Descriptor 발견만으로 ready를 판정하지 않는다.
- 세부 동작: [Host maintenance §5](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### RL-A5 Provider lifecycle을 반복해도 current target에 수렴한다

우선순위: `P2`

Provider가 반복 종료·재시작되어도 이전 lifecycle이 ready 목록에 누적되면 안 된다.

**검증 질문:** Provider B를 다섯 번 재시작해도 A와 current B만 requests를 처리하는가.

- 시작 조건: A와 B가 ready다.
- 절차: B Shutdown, status 제거, B replacement ready를 다섯 번 반복한다. 매 down 구간에는 A로 requests를
  보내고, B ready 뒤 A weight를 0으로 바꿔 B directed request를 확인한 다음 복원한다.
- 검증: Down 구간 requests는 A가 처리하고 각 replacement verification은 current B가 처리한다. Public
  status에는 이전 B RID가 ready로 남지 않는다.
- 세부 동작: [Runtime monitoring §3](../spec/24-runtime-monitoring.ko.md)을
  검증한다.

### Track B — In-flight operation과 신규 admission을 구분

#### RL-B1 Client cancellation 뒤 후속 request를 처리한다

우선순위: `P1`

Caller가 pending request await를 취소해도 late server reply가 다음 request를 완료해서는 안 된다.

**검증 질문:** Cancelled request 뒤 같은 client의 새 request가 자기 reply만 받는가.

- 시작 조건: Provider handler가 first request reply를 application gate에서 보류한다.
- 절차: First request가 handler에 도착한 뒤 caller await를 취소한다. 새 operation ID로 request를 보내 reply를
  받고 first gate를 해제한다.
- 검증: First awaitable은 cancellation이고 second request는 자기 payload reply를 한 번 받는다. Late first
  reply가 second completion을 바꾸지 않는다.
- 세부 동작: [오류 모델 §5](../spec/32-framework-error-model.ko.md)를 검증한다.

#### RL-B2 In-flight handler 중 provider crash를 처리한다

우선순위: `P1`

Provider가 request를 수락한 뒤 crash하면 처리 여부가 불명확할 수 있다. Framework는 같은 operation을 다른
provider에 자동 재제출하지 않는다.

**검증 질문:** Crash한 request가 terminal 하나로 끝나고 follow-up만 다른 provider에서 처리되는가.

- 시작 조건: A와 B가 ready이고 slow request가 B handler에 들어갔다.
- 절차: B를 강제 종료하고 request terminal을 기다린다. Client가 새 operation ID로 follow-up request를
  보낸다.
- 검증: Old request는 `Unavailable` 또는 `DeadlineExceeded` 중 하나로 한 번 끝난다. Same ID는 A에서
  처리되지 않고 follow-up만 A에서 한 번 처리된다.
- 세부 동작: [Failover policy §2](../spec/31-failure-failover-policy.ko.md)을 검증한다.

#### RL-B3 Graceful Shutdown 뒤 topology에서 제거한다

우선순위: `P1`

정상 종료는 신규 target selection을 중단하고 accepted request를 bounded하게 끝낸 뒤 current topology에서
빠져야 한다.

**검증 질문:** Provider B Shutdown 뒤 ready targets에서 B가 빠지고 accepted reply는 유지되는가.

- 시작 조건: Slow request가 B handler에 accepted되어 있고 A도 ready다.
- 절차: B Shutdown을 시작하고 새 requests를 보낸 뒤 slow handler gate를 해제한다.
- 검증: Accepted request는 B reply로 한 번 완료한다. Seal 뒤 신규 requests는 A가 처리하고 B terminal 뒤
  public status에는 B가 ready target으로 남지 않는다.
- 세부 동작: [Host maintenance §10](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### RL-B4 Runtime weight 0으로 신규 selection에서 제외하고 복원한다

우선순위: `P0`

Channel weight 0은 해당 membership만 신규 select-one에서 제외하며 process와 connection을 종료하지 않는다.

**검증 질문:** Weight 0 전파 뒤 B가 신규 request를 받지 않고 100 복원 뒤 다시 처리하는가.

- 시작 조건: A와 B가 weight 100으로 ready다.
- 절차: B weight를 0으로 바꾸고 public status 전파 뒤 requests 50개를 보낸다. B weight를 100으로 복원하고
  A weight를 0으로 바꾼 directed verification request를 보낸 뒤 A도 복원한다.
- 검증: First 구간은 A만 처리하고 directed verification은 B가 처리한다. B process와 connection은 전체
  구간 유지된다.
- 세부 동작: [Channel topology §4.3](../spec/07-channel-topology.ko.md)을
  검증한다.

#### RL-B5 Weight 변경 전에 accepted된 request를 완료한다

우선순위: `P0`

Weight update는 신규 target selection만 바꾸며 이미 B handler가 수락한 request를 취소하지 않는다.

**검증 질문:** B weight를 0으로 바꿔도 기존 slow request는 B reply로 완료되는가.

- 시작 조건: Slow request가 B handler에 들어가 reply gate에서 대기한다.
- 절차: B weight를 0으로 변경하고 신규 requests가 A에서 처리되는 것을 확인한 뒤 gate를 해제한다.
- 검증: Slow request는 B reply를 한 번 받고 신규 requests는 A가 처리한다.
- 세부 동작: [Channel topology §5.1](../spec/07-channel-topology.ko.md)의
  connection 유지와 weight update를 검증한다.

#### RL-B6 한 provider의 gray failure를 다른 replies와 격리한다

우선순위: `P1`

Provider 하나가 일부 input에서 error 또는 timeout을 내도 다른 provider의 correlation과 payload가 섞이지
않아야 한다.

**검증 질문:** A 정상 replies와 B의 deterministic error·timeout이 request별로 정확히 대응하는가.

- 시작 조건: A는 정상, B는 marker parity에 따라 `InternalFailure` 또는 delayed timeout을 반환한다. Weight는
  같다.
- 절차: 고유 markers의 requests 200개를 bounded concurrency로 보낸다.
- 검증: A가 처리한 IDs는 정상 reply, B가 처리한 IDs는 configured error 또는 timeout으로 한 번씩 끝난다.
  Handler evidence가 없는 target으로 자동 재제출되지 않는다.
- 세부 동작: [Channel messaging](../spec/08-channel-messaging.ko.md)과
  [오류 모델](../spec/32-framework-error-model.ko.md)의 request correlation과 provider failure 격리를 검증한다.

### Track C — 종료와 Store lifecycle 뒤 public resource 정리

#### RL-C1 다량 connection 뒤 정상 종료한다

우선순위: `P1`

Resource cleanup 내부 count는 E2E public contract가 아니다. E2E에서는 모든 public operations가 terminal에
도달하고 process가 bounded하게 종료되며 replacement가 같은 ports를 사용할 수 있는지 확인한다.

**검증 질문:** Load 뒤 Shutdown이 완료되고 replacement가 같은 listeners로 정상 시작하는가.

- 시작 조건: 많은 Stream connections와 requests가 완료된 상태다.
- 절차: Clients와 server를 public close·Shutdown으로 종료한다. Process exit 뒤 같은 ports로 replacement를
  시작한다.
- 검증: Pending public operations가 없고 old process가 종료되며 replacement listeners가 ready가 된다.
- 세부 동작: [Host maintenance §10](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### RL-C2 Crash한 provider를 owner lease 만료 뒤 제외한다

우선순위: `P2`

Descriptor remove를 수행하지 못한 crash도 current lease가 만료되면 ready target에서 빠져야 한다.

**검증 질문:** Provider B crash 뒤 A만 follow-up requests를 처리하는가.

- 시작 조건: A와 B가 ready다.
- 절차: B를 강제 종료하고 configured lease·status convergence를 기다린 뒤 requests 20개를 보낸다.
- 검증: Public status에는 B가 ready로 없고 A가 20개를 처리한다.
- 세부 동작: [Location runtime §5](../spec/21-location-runtime.ko.md)를
  검증한다.

#### RL-C3 정상 restart 뒤 새 lifecycle로 수렴한다

우선순위: `P2`

정상 process restart는 old ready identity를 제거하고 replacement identity를 current로 만든다.

**검증 질문:** Restart 뒤 old RID가 ready 목록에서 빠지고 replacement가 requests를 처리하는가.

- 시작 조건: Provider가 ready다.
- 절차: Provider Shutdown terminal을 확인하고 같은 endpoint에서 replacement를 시작한다.
- 검증: Public status에는 replacement RID만 ready이고 follow-up requests가 모두 replacement evidence에
  기록된다.
- 세부 동작: [Runtime monitoring §2.2](../spec/24-runtime-monitoring.ko.md)를 검증한다.

#### RL-C4 Location Store restart 중 existing messaging을 유지한다

우선순위: `P1`

Established connection의 liveness는 Store polling과 별개다.

**검증 질문:** Store restart 중 existing Channel requests가 계속 처리되고 복구 뒤 status가 Ready인가.

- 시작 조건: A와 consumer connection이 ready다.
- 절차: 지속 requests 중 Location Store를 restart한다. Public status가 degraded와 ready로 수렴하는 동안
  results를 수집한다.
- 검증: Existing route requests는 terminal을 하나씩 받고 복구 뒤 follow-up이 성공한다. Store failure를
  target missing으로 바꾸지 않는다.
- 세부 동작: [Transport liveness §7](../spec/29-transport-liveness.ko.md)을
  검증한다.

### Track D — 부하와 observability failure를 격리

#### RL-D1 High fanout에서 subscriber를 서로 격리한다

우선순위: `P2`

모호한 “높은 부하에서 안정적” 대신 subscriber 수와 application marker를 고정한다. Classic fanout의
subscriber 간 순서와 lossless delivery는 이 scenario의 계약이 아니다.

**검증 질문:** Ready subscriber 20개가 같은 marker를 독립적으로 관찰하고 한 subscriber의 처리가 다른
subscriber를 막지 않는가.

- 시작 조건: 20 subscribers가 publisher를 ready로 보고 있다. 한 subscriber의 `fanout-start` handler만
  application gate에서 대기시키고 나머지 gate는 열어 둔다. 시작 marker는 작게 유지하며 network block은
  넣지 않는다.
- 절차: Publisher가 `fanout-start` marker를 보낸 뒤 선택한 subscriber의 gate가 대기 중인 것을 확인하고
  bounded rate로 부하 event를 보낸다. 다른 subscribers의 marker evidence를 확인한 뒤 gate를 연다.
- 검증: 각 subscriber evidence가 `fanout-start` marker를 기록하고, 한 subscriber의 처리 지연이 다른
  subscriber의 public ready 상태나 marker 처리를 막지 않는다. Event sequence의 완전성·순서와 drop 수는
  판정하지 않는다.
- 세부 동작: [Framework API §11](../spec/06-framework-api.ko.md)을 검증한다.

#### RL-D2 Observer failure를 messaging에서 격리한다

우선순위: `P1`

Public message-flow observer가 exception을 던져도 handler dispatch는 계속되어야 하고 runtime error sink가
observer failure를 보고해야 한다.

**검증 질문:** Failing observer 뒤 normal requests가 성공하고 error sink event가 한 번 발생하는가.

- 시작 조건: Observer와 public runtime error sink를 등록한다.
- 절차: 첫 observer callback에서 exception을 발생시키고 normal requests 20개를 보낸다.
- 검증: 20 replies가 모두 성공하고 error sink가 정식 `observer_failed` event를 한 번 제공한다. Event에는
  payload와 exception object를 넣지 않는다.
- 세부 동작: [Message flow tracing §5](../spec/26-message-flow-tracing.ko.md)을
  검증한다.

#### RL-D3 Public logging sink에서 dispatch error를 확인한다

우선순위: `P1`

Dispatch error log가 관측 계약이라면 implementation-specific 문자열이 아니라 정식 field로 판정해야 한다.

**검증 질문:** Handler 없는 request가 public logging sink에 정식 error fields를 남기는가.

- 시작 조건: Logging sink와 normal handler를 등록한다.
- 절차: Missing handler request와 normal request를 각각 한 번 보낸다.
- 검증: Sink는 negative operation의 `dispatch_error`, `no_handler`, `reply_error` fields를 제공하고 normal
  request는 성공한다.
- 세부 동작: [Runtime monitoring §5](../spec/24-runtime-monitoring.ko.md)를 검증한다.

#### RL-D4 Same-version peers가 public error kind를 보존한다

우선순위: `P2`

E2E는 raw reply frame과 JSON key를 직접 읽지 않는다. Caller가 받은 public exception의 kind와 message를
server가 만든 error와 대조한다.

**검증 질문:** 각 server failure가 caller에서 같은 public ErrorKind와 application-safe message로
복원되는가.

- 시작 조건: Same-version provider가 `NotFound`, `Rejected`, `Unavailable`과 `InternalFailure` variants를
  만들 수 있다.
- 절차: Caller가 각 variant request를 한 번 보낸다.
- 검증: Caller의 public error kind와 message가 expected variant와 일치하고 success request는 정상 reply를
  받는다. Raw envelope는 contract test가 검증한다.
- 세부 동작: [오류 모델 §2](../spec/32-framework-error-model.ko.md)를 검증한다.

#### RL-D5 Fixed soak 중 lifecycle checkpoints를 통과한다

우선순위: `P2`

Soak은 환경별 성능 기준을 새 계약으로 만들지 않고 반복 lifecycle에서 functional correctness가 유지되는지
확인한다.

**검증 질문:** 5분 혼합 workload의 각 scale·weight·shutdown checkpoint 뒤 requests가 계속 terminal을
가지는가.

- 시작 조건: Client 20개가 request와 send를 초당 5개씩 시작한다.
- 절차: 1분 scale-out, 2분 provider weight 0, 3분 weight 복원, 4분 graceful scale-in을 실행한다.
- 검증: 각 checkpoint의 public status와 directed request가 expected target set을 확인한다. Requests는
  terminal 하나를 가지며 기능 오류와 pending 누적이 없다. Throughput·latency는 기록만 하고 공통 PASS
  threshold로 사용하지 않는다.
- 세부 동작: [Transport liveness](../spec/29-transport-liveness.ko.md)의 반복 lifecycle 수렴을 검증한다.

### Track E — Service connection liveness를 검증

#### RL-E1 Orderly disconnect를 peer deadline 전에 반영한다

우선순위: `P0`

FIN, RST와 정상 close는 15초 liveness deadline을 기다리지 않고 ready selection에서 제외해야 한다.

**검증 질문:** Orderly close와 RST 뒤 affected target만 common observation budget 안에 not-ready가 되는가.

- 시작 조건: RouteMesh와 ClientServer targets가 각각 ready다.
- 절차: Normal close와 RST variant를 fresh connections에서 실행하고 public status를 관찰한다.
- 검증: Affected connection은 fixed peer deadline 전에 ready target에서 빠지고 다른 target은 request를 계속
  처리한다.
- 세부 동작: [Transport liveness §5](../spec/29-transport-liveness.ko.md)을 검증한다.

#### RL-E2 Half-open connection을 application traffic과 독립적으로 판정한다

우선순위: `P0`

한 방향 traffic이 계속 보여도 Framework의 liveness round trip이 실패하면 connection은 not-ready가 되어야
한다.

**검증 질문:** Packet blackhole 뒤 15초 deadline에서 affected connection만 not-ready가 되는가.

- 시작 조건: Two targets가 ready이고 fault proxy가 한 connection 방향을 차단할 수 있다.
- 절차: A→B packet을 차단하고 B→A application traffic은 유지한다. Public status가 변할 때까지 fixed
  liveness deadline과 tolerance로 기다린다.
- 검증: Blocked connection만 not-ready이며 reverse application traffic이 deadline을 연장하지 않는다. 다른
  target requests는 성공한다.
- 세부 동작: [Transport liveness §3](../spec/29-transport-liveness.ko.md)를
  검증한다.

#### RL-E3 Reconnect 전 old reply가 새 request를 완료하지 않는다

우선순위: `P0`

Old physical connection의 delayed reply는 replacement connection의 operation과 correlation이 다르다.

**검증 질문:** Old request reply를 지연한 채 reconnect해도 new request는 자기 reply만 받는가.

- 시작 조건: Old provider handler가 request reply를 application gate에서 보류한다.
- 절차: Request entered 뒤 connection을 끊고 replacement를 ready로 만든다. New request를 완료한 뒤 old
  reply gate를 해제한다.
- 검증: Old request는 failure 또는 timeout terminal 하나를 유지한다. New request는 replacement reply를
  한 번 받고 old payload로 바뀌지 않는다.
- 세부 동작: [Transport liveness §6](../spec/29-transport-liveness.ko.md)을
  검증한다.

#### RL-E4 Connection loss 경쟁에서도 terminal 하나를 만든다

우선순위: `P0`

Admission, cancellation, disconnect와 reply가 가까이 발생해도 request completion은 한 번이어야 한다.

**검증 질문:** 세 race variants에서 operation terminal이 하나이고 handler 실행이 최대 한 번인가.

- 시작 조건: Handler entered와 reply release를 application signals로 제어한다.
- 절차: Admission 전, handler entered 뒤와 reply 직전에 connection loss·cancellation을 각각 경쟁시킨다.
- 검증: 각 request는 reply, cancellation, `Unavailable` 또는 timeout 중 하나로 한 번 끝난다. Same operation
  ID를 다른 provider가 처리하지 않는다.
- 세부 동작: [오류 모델 §5](../spec/32-framework-error-model.ko.md)를 검증한다.

#### RL-E5 Store 장애와 transport liveness를 독립적으로 처리한다

우선순위: `P1`

Store fail-static은 established connection의 liveness deadline을 중단하지 않는다.

**검증 질문:** Store 응답 중단과 packet blackhole이 함께 있어도 connection이 not-ready가 되고 Shutdown 뒤
재연결하지 않는가.

- 시작 조건: Ready connection과 current provider가 있다.
- 절차: Store response를 중단하고 connection packets도 blackhole한다. Public status에서 liveness failure를
  확인한 뒤 Host Shutdown을 완료한다.
- 검증: Connection은 peer deadline에서 not-ready이고 Store 오류가 이를 막지 않는다. Host terminal 뒤
  public status가 다시 Connecting으로 바뀌거나 new handler evidence가 생기지 않는다.
- 세부 동작: [Transport liveness §7](../spec/29-transport-liveness.ko.md)을
  검증한다.

### Track F — Relocation lifecycle과 fencing을 public 결과로 확인

#### RL-F1 Capacity가 바뀐 preflight에서 source를 보존한다

우선순위: `P0`

Preflight 뒤 target capacity가 부족해지거나 target이 unavailable이면 source admission을 복원해야 한다.

**검증 질문:** Target capacity·availability race에서 blocked Relocate 뒤 source request가 성공하는가.

- 시작 조건: Target capacity의 마지막 slot을 다른 create operation과 application gate로 경쟁시킬 수 있다.
- 절차: Relocate와 competing create를 함께 시작하는 capacity variant, target process를 precommit에 종료하는
  availability variant를 실행한다.
- 검증: Relocate가 성공한 경우 state는 target에 한 번 존재한다. Blocked variant는 source location과 state를
  유지하고 follow-up request가 성공하며 다른 target으로 자동 전환하지 않는다.
- 세부 동작: [Host maintenance §6](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### RL-F2 Rebind 뒤 이전 Session message를 새 binding에 적용하지 않는다

우선순위: `P0`

Actor owner가 A→B→A로 바뀌더라도 이전 Session binding token은 새 binding과 다른 identity다.

**검증 질문:** Old Session의 delayed relay·unbind가 new binding과 Actor state를 바꾸지 않는가.

- 시작 조건: Actor가 Session S1에 bind된 상태에서 target owner로 이동하고 새 Session S2에 rebind한다.
- 절차: S1 connection의 relay와 unbind를 network gate에서 지연시킨 뒤 S2 binding 완료 후 전달한다. S2에서
  normal relay와 push를 실행한다.
- 검증: Old operations는 stale binding result로 끝나고 Actor handler evidence가 없다. S2 relay와 push는
  한 번씩 성공하며 current binding은 S2다.
- 세부 동작: [Session Actor dispatch §4](../spec/20-session-actor-dispatch.ko.md)를
  검증한다.

#### RL-F3 Cross-language terminal failure를 같게 해석한다

우선순위: `P0`

Source와 target 언어가 달라도 정식 public ErrorKind와 typed `Rejected` 의미는 같아야 한다.

**검증 질문:** 방향이 있는 언어 조합에서 같은 failure scenario가 같은 public terminal을 반환하는가.

- 시작 조건: 각 target 언어가 spec의 error variants를 public handler로 제공한다.
- 절차: 모든 지원 언어 방향에서 정상 reply, `Rejected`, `NotFound`, `Unavailable`, timeout을 실행한다.
- 검증: Caller가 받은 terminal kind와 application payload가 scenario와 일치한다. Raw unknown code 주입은
  protocol contract test 책임이다.
- 세부 동작: [오류 모델 §8](../spec/32-framework-error-model.ko.md)을 검증한다.

#### RL-F4 Client role이 없는 ClientServer process는 outbound 호출하지 못한다

우선순위: `P0`

ClientServer Server role만 등록한 process는 같은 ChannelName의 Client egress를 갖지 않는다.

**검증 질문:** Server-only process의 request는 `NotFound`이고 정상 Client request는 성공하는가.

- 시작 조건: Server-only process와 별도 Client role process가 ready다.
- 절차: 두 process가 같은 ChannelName request를 각각 시작한다.
- 검증: Server-only call은 `NotFound`이고 handler가 실행되지 않는다. Client call은 server handler에서 한 번
  처리된다.
- 세부 동작: [ClientServer Channel §3](../spec/09-client-server-channel.ko.md)을
  검증한다.

#### RL-F5 Relocation 중 받은 messages를 target에서 순서대로 처리한다

우선순위: `P0`

Target restore가 진행 중이면 incoming messages를 application handler에 바로 전달하지 않고 handoff가 끝난
뒤 preserved work 다음에 처리해야 한다.

**검증 질문:** Restore-held 중 보낸 messages가 relocation completion 뒤 target에서 한 번씩 순서대로
처리되는가.

- 시작 조건: Source Actor·Instance Spot에 queued markers Q1·Q2가 있고 target adapter restore를 application
  gate에서 보류한다.
- 절차: Relocate를 시작하여 restore-held를 확인하고 H1·H2를 같은 logical IDs로 보낸다. Gate를 해제한다.
- 검증: Target handler evidence는 `Q1,Q2,H1,H2` 순서이고 각 marker가 한 번만 나타난다. Restore-held
  구간에는 application handler evidence가 없다.
- 세부 동작: [Location runtime §8](../spec/21-location-runtime.ko.md)과
  [Graceful drain과 handoff §8](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### RL-F6 Runtime mutable update와 invalid mutation을 구분한다

우선순위: `P0`

Weight처럼 spec이 허용한 값은 connection을 유지한 채 바꿀 수 있지만 identity·capability는 runtime update
대상이 아니다.

**검증 질문:** Public weight update는 selection을 바꾸고 unsupported mutation은 local validation error인가.

- 시작 조건: Two providers가 ready다.
- 절차: B weight를 0·100으로 변경하여 directed requests를 확인한다. 별도 invalid public configuration
  update를 시도한다.
- 검증: Weight update는 RL-B4 결과를 만들고 connection은 유지된다. Unsupported mutation은 operation을
  시작하기 전 public validation error이며 current target selection이 바뀌지 않는다.
- 세부 동작: [Channel topology §4.3](../spec/07-channel-topology.ko.md)을
  검증한다.

#### RL-F7 Relocated accepted request는 terminal 하나를 반환한다

우선순위: `P0`

Relocation 전에 accepted된 request의 reply가 connection loss와 겹쳐도 caller completion은 한 번이어야 한다.

**검증 질문:** Accepted request 중 Actor를 이동하고 source connection을 끊어도 request가 terminal 하나인가.

- 시작 조건: Actor handler가 request를 accepted하고 reply gate에서 대기한다.
- 절차: Actor Relocate를 시작하고 target에서 handler state가 복원된 뒤 caller route를 일시 차단한다. Reply
  gate와 route를 복구한다.
- 검증: Caller는 reply, timeout 또는 unavailable result 중 하나로 한 번 끝난다. Same operation ID가
  application handler에서 중복 실행되지 않고 follow-up request는 current target에서 성공한다.
- 세부 동작: [Spot actor §8](../spec/15-spot-actor.ko.md)을 검증한다.

#### RL-F8 Manual topology에서는 Host Relocate를 시작하지 않는다

우선순위: `P0`

Manual RouteMesh peer, ClientServer client endpoint 또는 manual fanout endpoint가 등록된 Host는
replacement readiness를 자동으로 확인할 수 없으므로 Host Relocate의 preflight에서 차단한다. 이 경우
source의 accepted work와 Host admission을 바꾸지 않는다.

**검증 질문:** Manual-only topology의 Relocate가 `ManualTopologyUnsupported`로 끝나고 source를 유지하는가.

- 시작 조건: Manual RouteMesh 또는 ClientServer endpoint만 가진 fresh Host에 stateful source object와
  application gate에서 대기하는 accepted request를 준비한다.
- 절차: Public Host Relocate를 호출하고 terminal을 확인한다. 이어서 request gate를 해제하고 source object의
  follow-up request를 보낸다.
- 검증: Relocate는 `Blocked/ManualTopologyUnsupported`이고 Host는 `Serving`을 유지한다. Accepted request와
  follow-up request는 source에서 한 번씩 처리되며 target restore·factory evidence는 없다.
- 세부 동작: [Host maintenance §4](../spec/28-graceful-drain-handoff.ko.md)와
  [§10](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### RL-F9 Preflight timeout과 post-seal deadline을 구분한다

우선순위: `P0`

Admission seal 전에 끝난 timeout은 source를 변경하지 않는다. Seal 뒤 teardown deadline은 forced terminal을
만들 수 있다.

**검증 질문:** Preflight-held와 closing-held variants가 서로 다른 public outcome·Host state를 반환하는가.

- 시작 조건: Target readiness gate와 source closing callback gate를 별도로 제공한다.
- 절차: 첫 Relocate는 readiness를 deadline 뒤까지 막고, 두 번째 fresh Host는 seal 뒤 closing callback을
  막는다.
- 검증: First는 `Blocked/DeadlineExceeded`이고 Host는 Serving이다. Second는
  `ForceStopped/DeadlineExceeded` 또는 spec의 post-seal forced outcome이며 source를 다시 Serving으로
  오인하지 않는다.
- 세부 동작: [Host maintenance §12](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### RL-F10 Entry Actor와 SpotWide aggregate를 Host Relocate한다

우선순위: `P0`

Host maintenance는 Application Join이 아니다. Entry Actor와 User Spot aggregate를 target에 복원하지만
Actor Join·Leave callbacks를 호출하지 않는다.

**검증 질문:** Relocate 뒤 state와 membership이 유지되고 Join·Leave callbacks는 실행되지 않는가.

- 시작 조건: Entry Actor와 `SpotWide` User Spot의 member Actors가 source에 있고 callback counters는 0이다.
- 절차: Host Relocate를 완료하고 current refs, state와 callbacks를 조회한다.
- 검증: Objects는 generation과 state를 유지해 target에서 request를 처리한다. Join·Leave callback counters는
  0이고 source Spot closing reason은 RelocationOut이다.
- 세부 동작: [Host maintenance §8](../spec/28-graceful-drain-handoff.ko.md)을
  검증한다.

#### RL-F11 Ready relocation units를 느린 units보다 먼저 완료한다

우선순위: `P0`

한 object의 current turn이 느리다고 Host의 다른 ready objects까지 모두 기다리게 하면 maintenance 중단이
커진다.

**검증 질문:** Slow units가 handler-held인 동안 ready units의 relocation completions가 진행되는가.

- 시작 조건: 80 objects 중 일부 handler를 application gates에서 보류하고 나머지는 idle이다.
- 절차: Host Relocate를 시작하고 ready object의 public locations를 관찰한 뒤 slow gates를 해제한다.
- 검증: 적어도 하나의 ready object가 slow gate 해제 전에 target location과 정상 handler result를 가진다.
  Slow objects도 해제 뒤 terminal에 도달하고 aggregate members는 함께 이동한다.
- 세부 동작: [Host maintenance §7](../spec/28-graceful-drain-handoff.ko.md)을
  검증한다.

#### RL-F12 User Spot queue와 timer를 relocation 뒤 복원한다

우선순위: `P0`

User Spot relocation은 queued messages와 logical timer schedule을 target에서 이어야 한다. Application이 timer를
다시 등록하지 않는다.

**검증 질문:** Frozen messages와 pending timer가 target에서 한 번씩 원래 순서로 처리되는가.

- 시작 조건: Source User Spot handler R0가 gate에서 대기하고 Q1·Q2, Actor A1·A2와 one-shot timer가 accepted된
  상태다.
- 절차: Relocate를 시작하고 R0를 해제한다. Completion 뒤 target evidence와 timer callback을 기다린다.
- 검증: R0 뒤 Q1·Q2와 A1·A2가 각 lane 순서를 유지해 한 번 처리된다. Timer callback도 target에서 한 번
  실행되고 Application이 timer registration을 반복하지 않는다.
- 세부 동작: [Spot actor §8](../spec/15-spot-actor.ko.md)을 검증한다.

#### RL-F13 많은 large-state units의 relocation을 bounded terminal로 끝낸다

우선순위: `P0`

Internal permit count는 E2E contract가 아니다. Public E2E는 많은 units와 size boundary에서 Host operation이
bounded하게 끝나고 source admission을 너무 일찍 막지 않는지 확인한다.

**검증 질문:** 80 units와 large-state variants가 success 또는 `StateIncompatible` terminal을 하나씩
가지는가.

- 시작 조건: 1 MiB state units 80개, 64 MiB boundary units와 oversize unit을 separate fixtures로 만든다.
- 절차: 각 fixture에서 Host Relocate를 실행하고 current locations와 operation results를 수집한다.
- 검증: Valid units는 target에서 checksum을 보존하고 oversize unit은 source를 유지한 채
  `StateIncompatible`이다. 모든 units와 Host operation은 bounded terminal을 가진다.
- 세부 동작: [Host maintenance §7](../spec/28-graceful-drain-handoff.ko.md)을
  검증한다.

#### RL-F14 Precommit abort 뒤 source queue 순서를 복원한다

우선순위: `P0`

Target reservation 또는 restore가 commit 전에 실패하면 frozen work와 seal 중 받은 work를 source에서
원래 순서로 다시 처리해야 한다.

**검증 질문:** Failed Relocate 뒤 source가 Q1·Q2·H1·H2를 한 번씩 순서대로 처리하는가.

- 시작 조건: Source User Spot에 frozen Q1·Q2가 있고 target adapter failure를 application marker로 선택할 수
  있다.
- 절차: Relocate를 시작하고 seal 구간에 H1·H2를 보낸다. Target reservation failure와 restore failure
  variants를 fresh objects에서 실행한다.
- 검증: Relocate는 blocked 또는 failed terminal이고 public current location은 source다. Source handler
  evidence는 `Q1,Q2,H1,H2` 순서이며 중복이 없다. Follow-up timer도 source에서 정상 실행된다.
- 세부 동작: [Host maintenance §9](../spec/28-graceful-drain-handoff.ko.md)을
  검증한다.

## 5. 완료 기준

- 모든 scenario는 public operation, status, object lookup과 application handler·callback evidence만 사용한다.
- Raw liveness command·ACK, invalid protocol frame, wire header, Core peer table와 internal relocation permit은
  E2E assertion이 아니다.
- Restart와 recovery는 latest public status와 directed follow-up request를 함께 확인한다.
- Process·network race는 application gates와 public readiness로 제어하고 fixed settle sleep이나 확률적 target
  선택에 의존하지 않는다.
- Operation마다 terminal result는 하나이며 connection 복구가 완료된 operation을 자동 재제출하지 않는다.
