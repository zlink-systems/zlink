# JVM service runtime regression test matrix

[Java 문서](../README.ko.md) · [Kotlin 문서](../../kotlin/README.ko.md) ·
[Runtime lifecycle](../../common/internals/README.ko.md)

Java와 Kotlin은 JVM service runtime 하나를 검증한다. Java public ABI와 Kotlin metadata·extension ABI는
각각 검사하지만 protocol state machine과 runtime E2E를 중복 구현하지 않는다.

## 1. Contract와 module 경계

- Java/Kotlin exact interface와 artifact ABI가 일치한다.
- JVM runtime은 Java binding의 exported public raw socket API만 호출한다.
- `runtime.nativeapi`, package-private member, reflection, JNI symbol과 Core private symbol 직접 호출은 0건이다.
- Core MeshNode, Spot, Actor, dispatch record와 STREAM session service type은 public ABI와 runtime dependency에 없다.
- 공통 protocol schema와 golden fixture hash가 C++·.NET·JVM·Node.js에서 같다.

## 2. Lifecycle과 maintenance

- `ApplicationVersion`은 non-negative Java/Kotlin `long`이며 target eligibility 비교가 공통 계약과 같다.
- `Retire`와 `Shutdown`의 effective intent, outcome과 reason wire 값이 공통 fixture와 같다.
- `Preparing`·`Error`의 `Retire`는 `Blocked/RuntimeNotReady`이며 admission을 바꾸지 않는다.
- `DisableRelocation` participant나 target capability가 없는 `Relocate`는 `Blocked/RelocationDisabled`다.
- `RecreateOnRelocation`과 `PreserveStateWith`는 같은 Location authority CAS와 Relocation Store publication 순서를 사용한다.
- `PreserveStateWith` adapter는 opaque `byte[]` application state만 받고 owner token, relocation reference와 phase를 받지 않는다.
- Instance Spot public local-only create와 existing-only resolve는 hidden remote `GetOrCreate`를 시작하지 않는다.
- Deadline, disconnect, reply와 shutdown 경쟁에서 terminal completion은 하나다.

## 3. Location과 recovery

- Authority Store read와 compare-exchange는 store version, lease와 store time을 한 결과로 반환한다.
- Owner와 relocation은 같은 authority row의 9개 phase를 사용하고 별도 relocation row를 만들지 않는다.
- Stale owner, coordinator와 lifecycle generation은 message, reply, timer와 phase write를 통과하지 못한다.
- Relocation payload `missing`과 idempotent delete가 닫힌 결과로 처리된다.
- 24시간 retention orphan이 active authority로 오인되지 않는다.

## 4. Transport liveness

- JVM runtime은 RouteMesh·ClientServer에 idle probe 5초와 inbound deadline 15초의 probe·ACK을 적용한다.
- Fanout publisher마다 전용 SUB socket과 receive loop를 사용하고, first valid receive에서 ready가 되며, idle
  publisher의 exact two-frame beacon은 application handler에 전달하지 않는다.
- Orderly disconnect는 timeout을 기다리지 않고 ready index에서 제거된다.
- Half-open peer는 15초 안에 not-ready가 되며 다른 ready peer와 host를 `Error`로 바꾸지 않는다.
- Reconnect는 admission을 다시 수행하고 이전 connection completion과 binding state를 재사용하지 않는다.
- Location owner lease와 service·fanout liveness를 같은 option이나 signal로 사용하지 않는다.

## 5. Java/Kotlin public entrypoint

- Java `CompletionStage`와 Kotlin `await()`가 같은 shared operation과 terminal result를 관찰한다.
- Coroutine cancellation은 waiter만 끝내고 runtime operation을 취소하지 않는다.
- Kotlin에 별도 lifecycle enum, termination wrapper, runtime facade와 relocation registry가 없다.
- Actor factory와 Snapshot state·adapter type mismatch는 socket bind 전 startup validation으로 끝난다.

## 6. E2E와 sample

- C++·.NET·JVM·Node.js의 `4 x 4` caller/server 조합이 공통 Channel, Spot, Actor, STREAM scenario를 통과한다.
- JVM lane은 Java와 Kotlin public entrypoint를 각각 compile하고 같은 runtime E2E에 연결한다.
- Sample은 public API만 사용하며 internal adapter나 raw frame 우회 코드를 포함하지 않는다.

## 7. Performance smoke

이 단계의 performance test는 runner와 package 연결이 실행 가능한지만 확인하는 smoke gate다. 수치를 사용한
성능 판정, baseline 비교, hotspot 분석과 tuning은 별도 performance 개선 작업에서 수행한다.

- 공통 perf runner와 JVM consumer가 clean build되고 현재 Framework·binding package와 Core runtime을 사용해
  시작하고 정상 종료한다.
- publish, request/reply와 양방향 send의 최소 workload가 각각 한 번 이상 성공한다.
- crash, hang, timeout과 terminal completion 중복이 없다.
- 결과에는 runtime/package version, artifact 절대 경로와 SHA-256, source revision, protocol·fixture revision,
  build mode와 성공·실패 수를 남긴다.
- 종료 뒤 process, thread·event-loop handle, timer, pending operation과 endpoint resource가 남지 않는다.
- smoke 수치를 release 성능 목표 충족 증거로 사용하지 않는다.
