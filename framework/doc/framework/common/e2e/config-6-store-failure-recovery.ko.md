<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: Resilience](config-5-resilience-lifecycle.ko.md) | [다음: Monitoring](config-7-monitoring.ko.md)
<!-- framework-adapter-nav:end -->

# Config 6 — Location Store와 Relocation Store 장애

Location Store는 service와 stateful object의 current 위치를 제공하고, Relocation Store는 이동 중인
Actor·Spot의 application state를 보존한다. Store가 잠시 응답하지 않아도 이미 ready인 transport connection을
즉시 끊지는 않는다. 반면 stateful owner는 마지막으로 확인한 owner lease deadline을 넘긴 뒤 신규 message를
계속 받아서는 안 된다.

이 config는 Store 중단·지연·복구와 provider crash를 실제 process로 만들고 public status, operation result와
application handler evidence를 확인한다. E2E client가 descriptor, owner lease, authority row와 relocation
chunk를 직접 읽거나 해석하지 않는다.

## 1. 확인 범위

- 정상 baseline과 polling 기반 automatic discovery
- Store 장애 중 기존 connection 유지와 신규 topology 변경 보류
- Owner lease 만료 뒤 stale provider 제외와 replacement
- 짧고 긴 Store 장애의 복구
- Store 응답 지연과 무관한 application 처리의 격리
- Relocation Store 장애, 장기 relocation과 owner replacement
- Public operational query pagination과 capacity 결과

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Location Store | 1 | Automatic topology, object location과 owner lease를 제공한다. Harness가 이 Store만 중지·지연할 수 있다. |
| Relocation Store | 1 | `PreserveStateWith` relocation payload를 제공한다. Location Store와 별도 instance·namespace로 장애를 제어한다. |
| Provider | 2 | Channel handler, Actor·User Spot·Instance Spot factory와 relocation adapter를 제공한다. |
| Consumer | 1 | Object Client와 Channel caller다. Public RouteMesh·Host status와 object operation endpoint를 제공한다. |
| E2E client | 1 | 역할 server의 public application endpoint만 호출한다. |

Owner lease renew interval, TTL, polling interval과 Store failure grace는 runner configuration에 명시한다.
시간 경계는 이 값과 tolerance에서 계산하며 임의의 settle sleep을 더하지 않는다. Store process와 provider
process의 stop·restart는 runner가 외부에서 수행한다.

## 3. 공통 실행과 판정 방법

각 scenario는 fresh Store namespace와 object IDs를 사용한다. Store 장애는 public status와 operation result로
확인하고, 복구 뒤에는 status ready와 실제 request 성공을 모두 확인한다. Status observer가 모든 중간
state를 전달한다고 가정하지 않고 마지막 `GetStatus`를 대조한다.

Application handler가 Store와 무관한 요청을 처리하는지 확인할 때는 Store proxy의 response gate를
application signal로 제어한다. Latency percentile의 작은 차이를 flaky threshold로 비교하지 않는다.

## 4. Scenario

### Track A — 정상 상태와 polling을 확인

#### SF-A1 Store 정상 상태 baseline

우선순위: `P0`

Store 장애 scenario를 판정하려면 먼저 같은 topology가 정상 상태에서 ready이고 request를 처리하는지
확인해야 한다.

**검증 질문:** 두 provider가 ready target으로 보이고 Channel request가 둘 중 하나에서 처리되는가.

- 시작 조건: Location Store, provider A·B와 consumer가 정상 실행 중이다.
- 절차: Consumer의 public RouteMesh status를 읽고 서로 다른 request 20개를 보낸다.
- 검증: Status는 ready target count 2를 제공하고 20개 request가 reply 하나씩 받는다. Handler count 합계는
  20이다.
- 세부 동작: [Location runtime §2](../spec/21-location-runtime.ko.md)을 검증한다.

#### SF-A2 Watch 없이 polling으로 provider 변경을 반영한다

우선순위: `P1`

Provider-specific watch가 없는 Location Store도 bounded snapshot polling으로 topology를 갱신할 수 있어야
한다.

**검증 질문:** Provider 추가·제거가 configured polling interval 안에서 public status와 request target에
반영되는가.

- 시작 조건: Polling만 제공하는 Store extension과 provider A가 ready다.
- 절차: Provider B를 시작하여 ready target 증가를 기다린다. B를 정상 종료하고 target 감소를 기다린다.
- 검증: B 추가 뒤 두 provider가 request를 처리할 수 있고 제거 뒤에는 A만 처리한다. 모든 대기는 polling
  interval과 common tolerance에서 계산한다.
- 세부 동작: [Location runtime §3](../spec/21-location-runtime.ko.md)을
  검증한다.

### Track B — Location Store 장애 중 fail-static 경계를 확인

#### SF-B1 Store 장애 중 기존 connection을 유지한다

우선순위: `P0`

Store polling 실패만으로 이미 ready인 transport가 끊기는 것은 아니다. Peer liveness가 정상인 동안 기존
connection으로 message를 계속 처리할 수 있다.

**검증 질문:** Location Store 중단 중에도 기존 provider request가 성공하는가.

- 시작 조건: SF-A1 baseline이 통과했고 두 peer가 ready다.
- 절차: 지속 request를 보내는 중 runner가 Location Store를 중지한다. Owner lease TTL보다 짧은 검증
  구간에 request 결과와 status를 읽는다.
- 검증: Existing connection의 requests는 계속 reply를 받는다. Status는 정식 degraded reason을 제공할 수
  있지만 ready transport를 Store 오류만으로 즉시 제거하지 않는다.
- 세부 동작: [Location runtime §4](../spec/21-location-runtime.ko.md)의
  fail-static 경계를 검증한다.

#### SF-B2 Failure grace를 넘겨도 기존 connection과 신규 discovery를 구분한다

우선순위: `P1`

Failure grace를 넘긴 뒤에는 Store에서 검증할 수 없는 신규 provider를 연결 대상으로 추가하지 않는다.
이미 ready인 transport는 자체 liveness가 정상인 동안 유지할 수 있다.

**검증 질문:** 장기 Store 장애 중 기존 request는 성공하고 새 provider는 target에 추가되지 않는가.

- 시작 조건: Provider A만 ready이고 failure grace가 명시되어 있다.
- 절차: Store를 grace보다 길게 중지하고 provider B를 시작한다. A에 request를 계속 보낸다.
- 검증: A requests는 성공하며 public status에 B가 ready target으로 추가되지 않는다. Store 복구 뒤에만 B가
  current target set에 들어간다.
- 세부 동작: [Location runtime §4](../spec/21-location-runtime.ko.md)를
  검증한다.

#### SF-B3 Discovery grace가 stateful owner lease를 연장하지 않는다

우선순위: `P0`

Discovery connection을 유지하는 grace와 Actor·Spot owner가 신규 업무를 받을 수 있는 lease는 다른
정책이다.

**검증 질문:** Transport는 유지되어도 owner lease deadline 뒤 Instance Spot 신규 request가 거부되는가.

- 시작 조건: Consumer와 provider connection은 ready이고 provider에 active Instance Spot과 periodic timer가
  있다. Failure grace는 owner lease TTL보다 길다.
- 절차: Store를 중지한 채 owner lease deadline을 넘긴다. 같은 Instance Spot에 request를 보내고 timer
  application evidence를 읽는다.
- 검증: RouteMesh peer는 transport liveness가 정상일 수 있지만 신규 stateful request는 정식 unavailable
  result로 끝난다. Lease deadline 뒤 timer callback evidence도 증가하지 않는다.
- 세부 동작: [Failover policy §5](../spec/31-failure-failover-policy.ko.md)을 검증한다.

### Track C — Stale provider와 lifecycle을 구분

#### SF-C1 Crash한 provider를 lease 만료 뒤 제외한다

우선순위: `P0`

Provider가 descriptor를 지우지 못하고 종료되어도 owner lease가 만료되면 current ready target으로 사용할 수
없다.

**검증 질문:** Provider B crash 뒤 ready target에서 B가 빠지고 follow-up request를 A만 처리하는가.

- 시작 조건: A와 B가 ready이고 각자 baseline request를 처리했다.
- 절차: Runner가 B를 강제 종료하고 public status가 current target set에 수렴할 때까지 기다린다. Follow-up
  request 20개를 보낸다.
- 검증: Status의 ready peer·target에는 B가 없고 20개를 A가 처리한다. B endpoint로 반복 timeout을 발생시켜
  성공으로 간주하지 않는다.
- 세부 동작: [Location runtime §5](../spec/21-location-runtime.ko.md)를
  검증한다.

#### SF-C2 정상 Shutdown은 lease expiry를 기다리지 않는다

우선순위: `P1`

정상 Shutdown은 신규 selection을 먼저 막고 owner 정보를 정리하므로 crash처럼 TTL 만료까지 기다릴 필요가
없다.

**검증 질문:** Provider B의 `Stopped/None` 직후 status에서 B가 제외되고 A가 계속 처리하는가.

- 시작 조건: A와 B가 ready다.
- 절차: B에 public Shutdown을 호출하고 Host terminal을 기다린다. 즉시 consumer status를 polling하고
  follow-up requests를 보낸다.
- 검증: B는 신규 target에서 빠지고 이미 accepted work만 bounded하게 끝낸다. Shutdown terminal 뒤 A가 모든
  follow-up request를 처리한다.
- 세부 동작: [Host maintenance §10](../spec/28-graceful-drain-handoff.ko.md)을 검증한다.

#### SF-C3 이전 owner lifecycle이 replacement를 바꾸지 못한다

우선순위: `P0`

같은 역할이 빠르게 재시작되어도 이전 process의 늦은 lease 동작이나 message가 새 lifecycle을 current에서
제외해서는 안 된다.

**검증 질문:** Paused old process를 재개해도 replacement가 ready 상태와 request 처리를 유지하는가.

- 시작 조건: Provider A를 process pause하여 lease가 만료되게 하고 replacement A2를 시작해 ready로 만든다.
- 절차: A2가 marker request를 처리한 뒤 old A를 재개한다. 서로 다른 marker의 requests를 계속 보낸다.
- 검증: Public status는 A2를 current ready peer로 유지하고 requests는 A2에서 한 번씩 처리된다. Old A의
  handler evidence는 증가하지 않는다.
- 세부 동작: [Failover policy §3](../spec/31-failure-failover-policy.ko.md)을 검증한다.

#### SF-C4 여러 service role을 가진 host를 한 lifecycle로 정리한다

우선순위: `P0`

한 process가 여러 MeshNode, ClientServer Server와 fanout publisher를 제공해도 host crash·restart 뒤 각 role은
같은 current lifecycle로 수렴해야 한다.

**검증 질문:** Multi-role host replacement 뒤 모든 역할이 새 process에서 ready가 되고 old 역할은
선택되지 않는가.

- 시작 조건: 한 host가 두 RouteMesh Channel, ClientServer Channel과 fanout publisher를 제공하고 모두
  ready다.
- 절차: Host를 강제 종료하고 replacement를 시작한다. 각 public status가 ready가 된 뒤 role별 marker를
  한 번 보낸다.
- 검증: 모든 marker를 replacement handler가 한 번 처리하고 old process evidence는 증가하지 않는다.
- 세부 동작: [Transport liveness §6](../spec/29-transport-liveness.ko.md)를
  검증한다.

#### SF-C5 Public operational query를 bounded page로 읽는다

우선순위: `P0`

Object가 많은 namespace를 한 번에 반환하면 응답 크기와 memory 사용이 무제한이 된다. Public operational
query는 page size와 continuation token을 사용해야 한다.

**검증 질문:** 1,001개 object를 page size 1·100·1000으로 읽어 중복·누락 없이 모두 얻는가.

- 시작 조건: Public manager operations로 1,001개 ready objects를 생성한다.
- 절차: 각 page size variant에서 첫 page부터 continuation이 끝날 때까지 public query를 반복한다.
- 검증: 각 page item 수는 요청 상한을 넘지 않고 전체 logical IDs는 1,001개로 정확하다. Continuation은
  client가 해석하거나 수정하지 않는다.
- 세부 동작: [Location runtime §7](../spec/21-location-runtime.ko.md)를 검증한다.

### Track D — Store 복구 뒤 current topology 수렴

#### SF-D1 짧은 장애는 기존 connection을 불필요하게 바꾸지 않는다

우선순위: `P0`

Failure grace 안에 Store가 복구되면 마지막 stable target set과 current connections를 유지한 채 fresh
snapshot으로 reconcile할 수 있다.

**검증 질문:** 짧은 Store 장애 전후로 같은 providers가 requests를 계속 처리하는가.

- 시작 조건: A와 B가 ready이고 지속 request workload가 실행 중이다.
- 절차: Store를 owner lease TTL보다 짧게 중지했다가 재시작한다. Status가 ready로 복구될 때까지 workload를
  유지한다.
- 검증: 모든 request가 terminal result를 하나씩 받고 current provider set은 A·B로 유지된다. Application
  evidence에 불필요한 re-registration 호출은 없다.
- 세부 동작: [Location runtime §6](../spec/21-location-runtime.ko.md)을 검증한다.

#### SF-D2 긴 장애 뒤 재등록한 provider만 유지한다

우선순위: `P0`

Store 장애가 lease TTL보다 길면 모든 이전 lease가 만료된다. 복구한 runtime은 자기 current lifecycle을
다시 게시하고, 장애 중 crash한 provider는 target set에서 제외해야 한다.

**검증 질문:** Long outage 복구 뒤 실행 중인 A는 유지되고 crash한 B만 제외되는가.

- 시작 조건: A와 B가 ready다.
- 절차: Store를 TTL보다 길게 중지하고 그 사이 B를 강제 종료한다. Store를 재시작하고 consumer status가
  수렴할 때까지 A requests를 보낸다.
- 검증: A requests는 가능한 구간에서 계속 성공하고 복구 뒤 ready target은 A 하나다. B를 replacement로
  자동 생성하거나 이전 route로 보내지 않는다.
- 세부 동작: [Location runtime §6](../spec/21-location-runtime.ko.md)을 검증한다.

#### SF-D3 Public status가 Ready·Degraded·Ready로 수렴한다

우선순위: `P1`

Application은 Store 장애와 복구를 public topology status에서 확인할 수 있어야 한다.

**검증 질문:** 한 장애 cycle의 latest status가 정상, degraded, 정상 상태를 순서대로 반영하는가.

- 시작 조건: Consumer observer가 initial Ready status를 받았다.
- 절차: Store를 중지하여 degraded status를 기다리고 다시 시작하여 Ready status를 기다린다.
- 검증: 같은 source에서 관찰한 sequence는 증가하고 각 단계의 마지막 `GetStatus`가 실제 Store·target
  상태와 일치한다. 모든 중간 event의 존재는 요구하지 않는다.
- 세부 동작: [Runtime monitoring §3](../spec/24-runtime-monitoring.ko.md)을
  검증한다.

### Track E — 느린 Store 응답을 application dispatch와 격리

#### SF-E1 Store response가 대기 중이어도 무관한 request를 처리한다

우선순위: `P1`

Store I/O가 느리다는 이유로 같은 process의 event loop나 application execution lane 전체가 멈추면 안 된다.

**검증 질문:** Store query가 response gate에서 대기하는 동안 Store와 무관한 Channel request가 완료되는가.

- 시작 조건: Store proxy가 특정 query response를 application signal에서 보류할 수 있고 normal Channel
  handler는 Store를 사용하지 않는다.
- 절차: Public operation으로 Store query를 시작하여 proxy-held를 확인한다. 그 상태에서 Channel request
  100개를 보내 모두 완료한 뒤 Store response를 해제한다.
- 검증: Channel requests는 Store gate 해제 전에 reply를 하나씩 받는다. Store operation도 해제 뒤 정식
  terminal을 반환한다.
- 세부 동작: [비동기 실행 정책 §2](../spec/05-async-execution-policy.ko.md)의 I/O 격리를
  검증한다.

### Track F — Relocation과 owner recovery의 public 결과를 확인

#### SF-F1 Cross-language object 위치와 state를 해석한다

우선순위: `P0`

한 언어 runtime이 만든 Actor·Instance Spot을 다른 언어 caller와 replacement runtime이 같은 global identity와
state로 사용해야 한다.

**검증 질문:** 방향이 있는 언어 조합에서 create·request·relocation 뒤 같은 application state를 얻는가.

- 시작 조건: Source와 target 언어가 같은 stable type과 public packet contract를 제공한다.
- 절차: Source 언어에서 object를 만들고 state를 변경한다. 다른 언어 caller가 request하고, target 언어로
  relocation한 뒤 다시 request한다.
- 검증: Public ID와 ObjectGeneration은 유지되고 payload·reply와 state 값이 모든 방향에서 같다.
- 세부 동작: [Public contract governance](../spec/00-public-contract-governance.ko.md)의 interop을 검증한다.

#### SF-F2 장기 relocation은 Store lease를 유지하고 실패 뒤 새 call을 허용한다

우선순위: `P0`

Capture·restore가 오래 걸려도 current relocation이 유효한 동안 완료할 수 있어야 한다. 중간 실패한
operation을 자동으로 재개하지 않고 Application이 시작한 새 call로 다시 시도한다.

**검증 질문:** Long-running relocation은 성공하고 실패 variant 뒤 새 relocation만 성공하는가.

- 시작 조건: Adapter capture를 application signal에서 장시간 보류할 수 있다.
- 절차: 첫 relocation을 Store retention보다 짧지만 여러 renew interval을 넘도록 유지한 뒤 해제한다.
  Fresh object의 second relocation은 Store fault로 실패시키고 복구 뒤 새 call을 시작한다.
- 검증: 첫 operation은 state를 보존하여 성공한다. Failed operation은 source location과 state를 유지하고
  복구 뒤 새 operation ID의 call만 target에서 완료한다.
- 세부 동작: [Relocation Store §5](../spec/23-relocation-store-redis.ko.md)를 검증한다.

#### SF-F3 Relocation Store 장애는 새 relocation만 막는다

우선순위: `P1`

Relocation Store를 사용할 수 없으면 payload를 보존할 수 없으므로 source를 변경하기 전에 operation을
끝내야 한다.

**검증 질문:** Store 장애 중 Relocate가 실패하고 source가 계속 request를 처리하는가.

- 시작 조건: Stateful object가 source에 ready이고 Location Store는 정상이다.
- 절차: Relocation Store만 중지하고 public Relocate를 호출한다. Terminal 뒤 source request를 보내고 Store
  복구 뒤 새 Relocate를 호출한다.
- 검증: 첫 call은 Store unavailable result이고 source request가 성공한다. 두 번째 call은 target에서
  state를 복원하며 첫 operation을 자동 재개하지 않는다.
- 세부 동작: [Relocation Store §7](../spec/23-relocation-store-redis.ko.md)을 검증한다.

#### SF-F4 ObjectGeneration과 owner replacement를 public ref로 구분한다

우선순위: `P0`

Relocation은 같은 logical incarnation을 유지하고 explicit close 뒤 recreate는 새 incarnation을 만든다.

**검증 질문:** Relocation은 ObjectGeneration을 유지하고 close·recreate는 더 높은 새 값을 반환하는가.

- 시작 조건: Actor와 Instance Spot의 initial public refs를 저장한다.
- 절차: 각 object를 다른 node로 relocate하여 ref를 다시 조회한다. 이후 public close·destroy하고 같은 ID로
  recreate한다.
- 검증: Relocation 뒤 generation은 initial과 같고 location만 target으로 바뀐다. Recreate ref는 다른
  nonzero generation이며 이전 exact ref lifecycle call은 current object를 바꾸지 않는다.
- 세부 동작: [Failover policy §4.1](../spec/31-failure-failover-policy.ko.md)을
  검증한다.

#### SF-F5 Creating owner crash 뒤 public request가 bounded recovery 결과를 얻는다

우선순위: `P0`

Instance Spot cold activation 중 owner가 crash해도 stale owner가 신규 업무를 받지 않아야 한다. 생성이
`Ready`가 되기 전에는 같은 generation의 생성 record를 계속 사용하거나 정확히 취소할 수 있다.

**검증 질문:** Creating owner crash 뒤 pending request와 follow-up request가 각각 terminal 하나를
가지는가.

- 시작 조건: Instance factory initialize를 application gate에서 보류한다.
- 절차: Cold request가 factory-held인 것을 확인하고 owner process를 강제 종료한다. Pending terminal을
  수집하고 recovery 조건이 갖춰진 뒤 새 request를 보낸다.
- 검증: Pending request는 success 또는 정식 failure 중 하나로 한 번 끝난다. Recovery가 같은 generation을
  계속하면 follow-up request는 그 결과에 합류하고, 취소되어 public resolve가 Ready object를 반환하지 않으면
  다음 call이 새 activation을 시작한다. 어느 경우에도 old owner evidence는 증가하지 않는다.
- 세부 동작: [Location runtime §6.1](../spec/21-location-runtime.ko.md)과
  [Failure와 failover §4.4](../spec/31-failure-failover-policy.ko.md)를
  검증한다.

#### SF-F6 Operational query 중 concurrent 변경을 다음 page cycle에 반영한다

우선순위: `P0`

Paged query 중 object가 추가·제거되어도 한 scan에서 duplicate ID를 반환하거나 continuation을 해석 오류로
끝내서는 안 된다.

**검증 질문:** Concurrent create·delete 중 각 completed scan이 bounded pages와 unique IDs를 반환하는가.

- 시작 조건: 1,001개 ready objects와 page size 100 query를 준비한다.
- 절차: 첫 page를 받은 뒤 일부 object를 create·delete하고 continuation을 끝까지 읽는다. 새 scan을 다시
  시작한다.
- 검증: 각 scan 안의 IDs는 중복이 없고 page 상한을 지킨다. 두 번째 scan은 완료된 current mutations를
  반영한다. Client는 continuation token을 수정하지 않는다.
- 세부 동작: [Location runtime §7](../spec/21-location-runtime.ko.md)를 검증한다.

#### SF-F7 Large state relocation은 public size limit 안에서 복원한다

우선순위: `P0`

Application state가 한 Store record보다 커도 Framework가 정식 relocation limit 안에서 payload를 보존하여
target에 복원해야 한다.

**검증 질문:** 64 MiB보다 큰 state를 가진 object가 relocation 뒤 같은 checksum과 logical length를
반환하는가.

- 시작 조건: Public application API로 deterministic large state를 만든다. 전체 크기는 spec의 logical
  relocation maximum보다 작다.
- 절차: Object를 target node로 Relocate하고 public request로 state checksum·length를 조회한다. 별도
  oversize fixture는 maximum을 넘긴다.
- 검증: 정상 state는 target에서 checksum·length가 같고 request를 처리한다. Oversize operation은 source를
  유지한 채 `Blocked/StateIncompatible`로 끝난다.
- 세부 동작: [Relocation Store §4](../spec/23-relocation-store-redis.ko.md)를 검증한다.

#### SF-F8 Target owner lease가 만료되면 source를 유지한다

우선순위: `P0`

Target preparation 중 target owner가 current가 아니게 되면 stale completion으로 commit해서는 안 된다.

**검증 질문:** Target process pause로 lease가 만료되면 Relocate가 실패하고 source handler가 계속
동작하는가.

- 시작 조건: Target adapter restore가 application gate에서 대기한다.
- 절차: Restore-held 뒤 target process를 pause하여 owner lease deadline을 넘긴다. Process를 재개하고 gate를
  해제한다.
- 검증: Relocate는 target unavailable result로 끝나고 public current location은 source다. Source follow-up
  request가 성공하며 target handler는 신규 workload를 받지 않는다.
- 세부 동작: [Failover policy §5](../spec/31-failure-failover-policy.ko.md)을 검증한다.

#### SF-F9 Old lifecycle cleanup이 replacement service roles를 제거하지 않는다

우선순위: `P0`

빠른 restart에서 old process의 늦은 cleanup이 replacement service descriptor를 삭제하면 stateless service가
다시 unavailable해진다. 이 scenario는 Stateful Actor·Spot owner를 자동으로 replacement하지 않는 정책과
분리한다.

**검증 질문:** Old process 재개 뒤에도 replacement Channel이 계속 선택되는가.

- 시작 조건: Old Channel provider를 pause하여 lease를 만료시키고 same-role replacement provider를 ready로
  만든다.
- 절차: Replacement Channel request를 확인한 뒤 old provider를 재개하고 같은 request를 반복한다.
- 검증: Public status는 replacement provider를 current ready target으로 유지하고 모든 follow-up marker를
  replacement가 한 번 처리한다. 이 scenario에서는 Actor·Spot object location을 판정하지 않는다.
- 세부 동작: [Failover policy §3](../spec/31-failure-failover-policy.ko.md)을 검증한다.

#### SF-F10 많은 accepted requests와 relocation completion을 함께 처리한다

우선순위: `P0`

Accepted request가 많은 object를 이동해도 각 request의 reply와 relocation terminal이 중복되거나 누락되면
안 된다.

**검증 질문:** In-flight requests와 large replies 중 relocation해도 operation마다 terminal 하나를
받는가.

- 시작 조건: Stateful object의 handler가 request별 application signal에서 reply를 보류한다.
- 절차: 서로 다른 IDs의 requests를 많이 수락시킨 뒤 Relocate를 시작하고 signals를 해제한다. Completion
  뒤 follow-up request를 보낸다.
- 검증: 각 accepted request는 reply, timeout 또는 relocation failure 중 하나로 한 번 끝난다. Relocate
  terminal도 하나이며 follow-up은 current target에서 한 번 처리된다.
- 세부 동작: [Host maintenance §7](../spec/28-graceful-drain-handoff.ko.md)을
  검증한다.

#### SF-F11 Cancellation과 response loss 뒤 payload 값을 보존한다

우선순위: `P0`

Store call waiter가 취소되거나 response가 유실되어도 mutable application payload를 다른 operation 값으로
재사용해서는 안 된다.

**검증 질문:** Cancelled operation 뒤 새 relocation이 자기 payload checksum만 target에 복원하는가.

- 시작 조건: 서로 다른 deterministic payload A와 B를 가진 two fresh objects를 준비한다.
- 절차: A relocation waiter를 Store response가 대기 중일 때 취소한다. Store를 정상화하고 B relocation을
  실행한다.
- 검증: A awaitable은 cancellation 결과를 유지한다. B target state checksum은 B와 정확히 같고 A bytes가
  섞이지 않는다. 각 operation은 terminal 하나를 가진다.
- 세부 동작: [Relocation Store §6](../spec/23-relocation-store-redis.ko.md)을 검증한다.

### Track G — Capacity 결과를 public create·relocation으로 검증

#### SF-G1 Actor·Spot·stable type limit을 atomic하게 적용한다

우선순위: `P0`

한 creation이 여러 capacity limit을 함께 사용하면 일부 limit만 소비한 채 실패해서는 안 된다.

**검증 질문:** Concurrent creates의 성공 수가 모든 public limit을 지키고 failed create 뒤 capacity가
복구되는가.

- 시작 조건: Actor total, Spot total과 stable type limit을 서로 다른 작은 양수로 설정한다.
- 절차: 여러 caller가 마지막 slot보다 많은 creates를 동시에 시작하고 일부 factory는 application error를
  반환한다. 실패 뒤 새 create를 시도한다.
- 검증: Successful active counts는 어떤 limit도 넘지 않는다. Capacity가 없는 calls는
  `CapacityExceeded`이고 factory-failed calls는 active count에 남지 않는다. Cleanup 뒤 새 create가
  available slot을 사용할 수 있다.
- 세부 동작: [MeshNode §5](../spec/13-mesh-node.ko.md)를 검증한다.

#### SF-G2 Unlimited population과 activation concurrency를 구분한다

우선순위: `P0`

Population limit 0은 unlimited이며 activation concurrency는 동시에 factory를 실행하는 수만 제한한다.

**검증 질문:** Unlimited population에서 active objects가 유지되고 factory concurrency만 configured limit을
지키는가.

- 시작 조건: Population limits는 0, activation concurrency는 작은 양수다. Factory는 application gate에서
  active count를 기록한다.
- 절차: 많은 creates를 동시에 시작하고 factory gate를 순차 해제한다.
- 검증: Factory active count는 concurrency limit을 넘지 않지만 모든 valid creates는 결국 성공한다.
  Entry Spot은 Spot population에 포함되지 않고 member Actors는 Actor count에 포함된다.
- 세부 동작: [MeshNode §5](../spec/13-mesh-node.ko.md)를 검증한다.

#### SF-G3 User Spot aggregate capacity를 all-or-none으로 적용한다

우선순위: `P0`

User Spot과 N member Actors를 함께 옮기려면 target에 Spot slot 하나와 Actor slots N개가 모두 있어야 한다.

**검증 질문:** 한 capacity bucket이라도 부족하면 source aggregate가 유지되고 모두 충분할 때만 target으로
이동하는가.

- 시작 조건: Spot slot 부족, Actor slot 부족, stable type slot 부족과 all-sufficient target variants를
  각각 준비한다.
- 절차: 같은 크기의 aggregate를 각 fresh target으로 Relocate한다.
- 검증: 부족 variants는 capacity blocker result이고 public locations와 state가 source에 유지된다.
  Sufficient variant는 Spot과 모든 Actors가 target에서 같은 state와 generations로 처리된다.
- 세부 동작: [Host maintenance §8.5](../spec/28-graceful-drain-handoff.ko.md)를
  검증한다.

## 5. 완료 기준

- 모든 판정은 public status, operation terminal, object lookup과 application handler·callback evidence를
  사용한다.
- Descriptor, lease token, authority row, Store version, relocation manifest·chunk와 provider buffer는 E2E
  assertion이 아니다.
- Store 장애·복구는 latest public status와 실제 follow-up request를 함께 확인한다.
- Time boundary는 configured lease·grace·poll interval에서 계산하고 arbitrary settle sleep을 사용하지 않는다.
- Store response delay는 application signal로 제어하며 작은 latency ratio로 pass/fail을 정하지 않는다.
