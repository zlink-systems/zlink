# Framework relocation·Auto HWM 구현 진행 문서

> 상태: 구현 진행 기준
>
> 이 문서는 새 공개 계약을 정의하지 않는다. 이미 확정한 relocation spec과 Auto HWM 계획을
> 실제 Core, bindings와 Framework 구현으로 연결하고 전체 sample 통과까지 진행하기 위한 단일
> 실행 문서다. 이 작업을 위해 별도의 plan이나 gap 문서를 추가하지 않는다.

## 1. Goal

개선된 Actor·Spot·Host relocation과 Auto HWM memory budget을 Core, bindings와 Framework
C++/.NET/Java/Kotlin/Node.js의 실제 production 경로에 반영한다. 특정 언어의 test adapter나
sample 전용 우회 경로로 구현하지 않는다.

각 단계에서 관련 contract test를 통과시키고, 마지막에는 모든 Framework 언어의 sample runner를
실행해 전체 sample이 통과해야 완료한다. 실패한 sample의 기대값을 낮추지 않고 원인을 소유한
runtime 계층을 수정한다.

## 2. 반드시 참조할 문서

### 2.1 Relocation 정식 계약

다음 문서는 relocation 구현의 기준이며 plan보다 우선한다.

1. [Actor와 Spot relocation 전체 흐름](../../framework/doc/framework/common/spec/28-relocation-flow.ko.md)
   - Source·target·Session 책임
   - Restore, ordered relay, one-way cutover와 target-only Location Store CAS
   - 1,000ms cutover fallback, Store retry, queue 병합과 failure 처리
2. [Host relocation 전체 흐름](../../framework/doc/framework/common/spec/30-host-relocation-flow.ko.md)
   - Host target 선택
   - Relocation inventory, unit, batch와 실행 순서
   - Host state, completion, shutdown과 failure
3. [Session Actor dispatch](../../framework/doc/framework/common/spec/20-session-actor-dispatch.ko.md)
   - Bound Session seal, route update, held message 제출과 connection 정리
4. [Location runtime](../../framework/doc/framework/common/spec/21-location-runtime.ko.md)
   - Location owner, generation, CAS와 Store 응답 불확정 처리
5. [Relocation handoff 상태 전이](../../framework/doc/framework/common/internals/13-relocation-handoff.ko.md)
   - 모든 언어가 공유할 내부 state, queue owner와 검증 경계

구현 중 계약 누락을 발견하면 위 문서를 임의로 바꾸지 않는다. 실제 충돌 위치와 필요한 결정을
보고한 뒤 승인된 범위만 같은 vertical slice에서 수정한다. 영문 spec은 한국어 계약과 같은 의미로
유지한다.

### 2.2 Auto HWM 구현 계획

다음 네 문서는 함께 사용한다.

1. [공통 계산과 책임](auto-hwm-memory-budget-and-ccu-recalculation-plan.ko.md)
2. [Core 구현](auto-hwm-memory-budget-core-plan.ko.md)
3. [Bindings 구현](auto-hwm-memory-budget-bindings-plan.ko.md)
4. [Framework 구현](auto-hwm-memory-budget-framework-plan.ko.md)

공통 계획은 값, 단위와 불변 조건의 기준이다. Core, bindings와 Framework 계획은 각 계층의 구현
위치와 검증 항목을 소유한다. Public API가 바뀌는 slice에서는 해당 정식 spec과 exact language
interface를 구현과 함께 수정한다. 구현하지 않을 후속 범위를 정식 spec에 미리 추가하지 않는다.

## 3. 변경할 수 없는 책임 경계

### 3.1 Relocation

- Source runtime은 현재 dispatch를 멈추고 기존 queue와 이전 주소로 들어오는 message를 target에
  relay한다. Source와 Session owner는 Location Store를 변경하지 않는다.
- Target runtime은 temporary queue와 Restore를 준비한다. Relay 준비 reply 뒤 source가 보낸
  one-way cutover를 받거나 1,000ms가 지나면 target만 Location Store CAS를 실행한다.
- Cutover timeout 뒤에는 Warning을 기록하고 진행한다. 늦거나 중복된 cutover는 state를 바꾸지 않고
  Warning만 기록한다.
- Location Store 응답이 불확정하면 target은 같은 fence로 read/retry한다. Restore 유효시간까지
  target owner를 확인하지 못하면 Error를 기록하고 준비한 Actor 또는 Spot과 queue를 제거한다.
- Target은 CAS 성공 뒤 저장된 작업, cutover 전 relay와 temporary queue를 정식 계약의 순서로 합치고
  application dispatch를 연다.
- Bound Actor라면 target runtime이 Session owner에 one-way route update를 보낸다. Session owner는
  exact physical Session과 binding만 확인하고 held message를 제출한 뒤 seal을 해제한다.
- `SessionRelocationSealTimeout` 기본값은 3,000ms다. 그 안에 exact route update가 없으면 Session
  owner가 physical connection, binding, held message와 seal을 정리한다.
- Actor와 Spot은 같은 handoff를 사용한다. Object별 차이는 factory, state adapter, membership과
  lifecycle callback에만 둔다.
- Relocation queue에 별도 record 수, participant 수 또는 byte correctness cap을 추가하지 않는다.
  일반 Core HWM과 connection backpressure를 사용한다.

### 3.2 Auto HWM

- Core context 하나가 Core-managed messaging budget 하나를 소유한다. Profile 비율, physical
  directional queue 수, water-filling, queue별 HWM과 accounting은 Core에서만 계산한다.
- Bindings는 값과 snapshot을 손실 없이 변환하고 managed runtime memory hint를 전달한다. 별도 budget을
  계산하지 않는다.
- Framework는 같은 Core option을 전달하며 별도 application budget을 만들지 않는다. Core queue에서
  Framework job으로 이동한 message는 origin queue의 retained credit을 job 완료까지 유지한다.
- 한 connection의 HWM 정체가 다른 RID의 submit과 언어 runtime thread를 막지 않게 한다.
- 빈 directional pipe의 complete oversize message 한 건 진행성, manual HWM과 completion progress
  규칙은 공통 계획을 그대로 적용한다.
- Auto HWM 변경을 relocation 전용 제한이나 별도 relay queue capacity로 변환하지 않는다.

## 4. 구현 순서

각 단계는 하나의 동작 가능한 vertical slice로 끝낸다. 같은 실패를 확인하기 위해 전체 suite를
반복하지 않고 focused test를 먼저 사용한다.

### 4.1 POSDDD refactoring 시점

기능이 아직 RED인 동안 구조를 넓게 바꾸지 않는다. 각 vertical slice가 production 경로에서
focused test를 통과한 직후, 다음 계층이나 다음 언어로 복제하기 전에 POSDDD refactoring을 한 번
수행한다.

Refactoring에서는
[POSDDD 원칙](../principal/dev/posddd.ko.md)과
[ZLink system 설계 원칙](../principal/dev/zlink-system-design-principles.ko.md)을 기준으로 다음을 확인한다.

- 같은 invariant와 protocol 지식이 여러 owner에 중복되어 있지 않은가
- 순서만 기준으로 분리한 class, pass-through method와 얕은 wrapper가 생기지 않았는가
- Relocation state, queue와 fence가 하나의 aggregate 밖으로 누출되지 않았는가
- Core budget 계산, binding 변환과 Framework lifecycle 책임이 서로 섞이지 않았는가
- 특정 언어나 sample만을 위한 branch, adapter와 test-only production hook이 남지 않았는가
- Public interface가 구현 세부 정보나 추가 사전 조건을 호출자에게 요구하지 않는가

Refactoring은 이미 통과한 contract를 바꾸지 않고 중복 state와 불필요한 interface를 줄이는 범위로
제한한다. 완료 후 해당 focused test를 다시 실행한 다음 다음 slice로 진행한다. 모든 언어 구현이 끝난
뒤에는 cross-language 이름, state와 error 의미를 비교하는 최종 POSDDD review를 한 번 수행한다.

### 4.2 `sol` review를 요청하는 조건

기본 구현은 `terra high`로 진행한다. 다음 조건 중 하나가 발생하면 같은 시도를 계속하지 말고
`gpt-5.6-sol`에 read-only review를 요청한다.

- 같은 root cause와 blocking condition이 세 번 연속 반복된다.
- 정식 spec만으로 state owner, linearization point 또는 failure 방향을 하나로 결정할 수 없다.
- Location Store CAS, queue ordering, Session seal 또는 retained credit의 concurrency 증거가 서로 충돌한다.
- 한 언어만 공통 contract와 다른 예외 경로를 요구하는 것처럼 보인다.
- 수정이 public API, wire contract 또는 두 개 이상의 계층 책임을 함께 바꿀 가능성이 있다.

Review 요청에는 전체 repository를 다시 읽히지 않는다. 다음 evidence packet만 제공한다.

1. Goal과 관련 spec section
2. 수정 범위의 file과 symbol
3. 최소 재현 명령과 첫 실패
4. 확인된 state transition 또는 accounting 값
5. 이미 시도한 변경과 실패 이유
6. 검토할 두 가지 이상 대안

`sol`에는 구현을 대신 맡기지 않고 책임 경계, 원인과 대안의 위험을 검토하도록 요청한다. Review 결과는
정식 spec보다 우선하지 않는다. 계약 변경이 필요하다는 결론이면 구현을 멈추고 정확한 충돌과 제안을
보고한 뒤 승인받는다.

### 단계 1. 현재 gap을 코드에서 확인한다

- 위 문서에 나온 state와 public option을 symbol 단위로 찾는다.
- 언어별로 이미 구현된 부분, 실제 production에서 우회하는 부분과 누락만 기록한다.
- 새 조사 문서나 전체 repository 목록을 만들지 않는다.

### 단계 2. Relocation 공통 흐름을 먼저 완성한다

1. Target temporary queue와 Restore 준비
2. Source의 cached queue·continuous relay와 one-way cutover
3. Target-only Location Store CAS와 Restore 유효시간까지 retry
4. Target queue 병합과 dispatch 개방
5. Bound Session route update, held 제출, seal 해제와 3,000ms timeout 정리
6. Actor, PerActor·SpotWide User Spot, Instance Spot과 standalone Actor adapter 연결
7. Host inventory, target 선택과 batch dependency 연결

각 언어는 같은 state와 failure 방향을 사용해야 한다. 기존 언어 구현을 다른 언어가 복사하는 것이
아니라 정식 spec을 공통 기준으로 사용한다.

### 단계 3. Core Auto HWM을 완성한다

1. Context memory 입력과 단일 Core budget
2. Physical directional queue registry와 byte accounting
3. Connection attach·detach 재계산과 deferred shrink
4. Provisional allocation credit, oversize 진행성과 wake
5. Retained-credit lease와 snapshot/reset
6. Binding 비동기 submit에 필요한 nonblocking Core 경계

### 단계 4. 모든 binding을 연결한다

- C++, .NET, Java, Node.js와 계획에 포함된 다른 binding이 같은 Core option, snapshot과 error를
  노출하도록 한다.
- Retained receive와 lease release를 정상 완료, drop, cancellation, exception, disconnect와 shutdown에
  연결한다.
- Routed send/request의 최초 수용 대기가 event loop, runtime worker 또는 socket 전체 submit lock을
  점유하지 않게 한다.

### 단계 5. 모든 Framework 언어를 연결한다

- C++, .NET, Java, Kotlin과 Node.js에서 같은 설정과 diagnostics를 binding에 전달한다.
- Framework가 따로 계산하는 application HWM과 retry queue를 계획의 순서에 따라 제거한다.
- Ingress job이 retained credit을 정확히 한 번 반환하도록 모든 terminal 경로를 연결한다.
- Relocation control도 일반 connection backpressure를 따르되 relocation state와 timeout 의미를
  변경하지 않는다.

### 단계 6. Contract와 sample을 수렴시킨다

- 수정 중에는 해당 기능의 focused unit/contract test만 실행한다.
- 한 언어의 production 경로가 완료되면 그 언어의 contract suite를 실행한다.
- 모든 언어가 완료된 뒤에만 전체 sample runner를 순차 실행한다. 동시에 여러 sample suite를 실행해
  port, Redis, CPU와 timeout이 서로 영향을 주게 하지 않는다.
- Sample 실패는 production code에서 수정한다. Public example이나 expected output을 삭제하거나
  timeout만 늘려 통과시키지 않는다.

## 5. 최종 sample gate

최종 단계에서는 repository가 제공하는 runner를 그대로 사용한다.

```bash
bash framework/languages/cpp/samples/run_samples.sh
bash framework/languages/dotnet/samples/run_samples.sh
bash framework/languages/java/samples/run_samples.sh
bash framework/languages/node/samples/run_samples.sh
```

Java runner가 Java와 Kotlin을 함께 실행하는지 runner inventory로 확인한다. 분리된 Kotlin 실행이
존재하면 같은 최종 gate에 포함한다. 각 runner가 시작한 process와 임시 resource를 종료한 뒤 다음
언어를 실행한다.

## 6. 완료 조건

다음 조건을 모두 만족해야 goal을 완료한다.

- Relocation sequence, CAS writer, queue order와 failure 방향이 정식 spec과 일치한다.
- Host relocation이 전체 inventory를 한 번씩 선택하고 정해진 batch dependency를 지킨다.
- Bound Session seal, held message, route update와 timeout 정리가 실제 production 경로에서 동작한다.
- Core budget과 physical queue HWM 계산이 단일 owner에서 수행된다.
- Bindings와 Framework가 별도 budget 또는 relocation-specific capacity를 만들지 않는다.
- C++, .NET, Java, Kotlin과 Node.js의 관련 contract test가 통과한다.
- 위 전체 sample runner가 모두 성공한다.
- Sample, contract expectation 또는 spec을 구현 실패를 숨기기 위해 완화하지 않는다.
- 임시 debug log, test-only production hook, 우회 adapter와 미정리 background process가 남지 않는다.
- 변경한 파일의 diff check가 통과하고 실제 실행 결과와 남은 제한을 최종 보고한다.

## 7. Goal 실행 prompt

```text
`doc/plan/framework-relocation-auto-hwm-execution.ko.md`를 단일 진행 문서로 사용해 끝까지 수행한다.
문서가 지정한 relocation spec과 Auto HWM 네 계획을 먼저 읽고, Core → bindings → Framework
C++/.NET/Java/Kotlin/Node.js의 실제 production 경로에 반영한다. Focused contract test로 각
vertical slice를 검증한 직후 POSDDD refactoring을 수행하고, 같은 원인의 실패가 세 번 반복되거나
ownership·concurrency 판단이 불명확하면 evidence packet으로 gpt-5.6-sol read-only review를 요청한다.
마지막에는 모든 Framework sample runner를 순차 실행한다. Sample과 기대값을 수정해 실패를 숨기지
말고 원인을 소유한 runtime에서 고친다. 문서의 완료 조건을 모두 만족할 때만 goal을 완료한다.
```
