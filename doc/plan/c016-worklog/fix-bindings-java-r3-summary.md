# Java binding R3 수정 결과

Java binding의 F-R3-1, F-R3-2, F-R3-12, F-R3-13, F-R3-14, F-R3-15를 수정했다.
공개 API는 변경하지 않았고 commit·push는 수행하지 않았다. 변경 범위는 `bindings/java/`와
이 보고서다. Core, 다른 binding, Framework, spec, site는 수정하지 않았다.

진단 기준은 [R3 리뷰](spec-review/R3-bindings-summary.md)와
[캠페인 결정](decisions.ko.md)의 D-098·D-109·D-111이다. 아래 원인 위치는 수정 전 파일 기준이며,
수정 위치는 최종 Java 파일 기준이다. 분류 B는 기존 계약을 위반한 구현 결함의 수정이다.

## F-R3-1 — WRITABLE의 RID 재검증

- 소유 계층: Core가 RID echo를 보장하고 Java completion owner는 socket-local context·token을 waiter에 연결한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:986`의 part send, `:1148`의 completion record;
  `bindings/doc/spec/README.ko.md:1340`의 Submit 결과 투영.
- 원인: `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:709`의 비교용
  `RoutingId` 생성과 `:1119`의 `completion.peer()` 비교.
- 수정: `readResult`, `WritableCompletion`, `captureWritable`에서 RID 읽기·보관·비교를 제거했다.
  kind·completion ID·context 확인은 유지한다.
- 교차언어 대조: C raw ABI `bindings/c/include/zlink/socket/api.h:225`는 Core completion을 그대로
  제공한다. R3에서 확인한 다른 고수준 binding의 RID 재검증은 각 언어 작업 범위다.
- 변경 분류: B. Core 계약을 만족하는 결과의 공개 동작은 동일하다.
- 수정 전/후 규칙 수: Java 범위에서 Core echo 보장 + Java 재판정 **2 → 1**.
- 회귀: `WritableTokenDeliveryContractTest.java:15`. 같은 RID의 두 waiter 중 전달된 token의
  waiter만 완료된다. 테스트 전용 ABI에서 RID echo만 비워 재판정이 사라졌는지도 검증한다.
  이는 Core가 잘못된 RID를 반환해도 된다는 공개 계약을 추가하는 테스트가 아니다.
- Diff 분리: `01-F-R3-1.patch` — 위 Owner의 RID 관련 hunk, 회귀 클래스,
  공용 테스트 fixture `CompletionNativeFixture.java`.
- Gate 결과: 신규 회귀 포함 7건 × 5회 모두 통과. 전체 gate는 아래 공통 결과 참조.
- BLOCKERS: 없음.

## F-R3-2 — 3-part 이상 routed receive의 직접 저장

- 소유 계층: Java `Received`의 기존 caller-provided 결과 저장소.
- Spec 조항: `bindings/doc/spec/README.ko.md:943`의 수신 저장소 계약;
  `bindings/doc/spec/java/README.ko.md`의 caller-provided `Received` 계약, D-B115·D-109.
- 원인: `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterReceiveSupport.java:209`의
  `Received fresh` 생성 후 `receivedAdoptFrom`; `Received.java:354`의 envelope 상태 재이동.
- 수정: 완성된 `Message[]`와 RID·reply metadata를 target에 직접 채운다. 1·2·3+ part 초기화가
  `Received.prepareRoutedStorage`를 공유하며 기존 part collection을 재사용한다. 3+ part 수신 또는
  metadata 준비 실패 시 이번 수신에서 확보한 모든 part를 정리한다. 다른 수신 경로가 쓰는
  `adoptFrom`은 유지한다. 추가한 접근 함수는 `ContractAccess`의 내부 bridge다.
- 교차언어 대조: Go `bindings/go/internal/native/received.go:48`의 `beginReceive`와 `:66`의
  `replace`는 caller 저장소에 part·metadata를 직접 설정한다. Java에는 D-B115 이후 3+ part
  fallback에만 중간 envelope가 남아 있었다.
- 변경 분류: B. 완성된 payload·metadata·reply 동작은 동일하다.
- 수정 전/후 규칙 수: 1·2 part 직접 저장 / 3+ part envelope 재채택 **2 → 1**.
- 회귀: `RouterReceiveStorageContractTest.java:18`은 1-part에서 3-part REQUEST, 다시 3-part DATA로
  같은 저장소를 채워 내부 collection identity, 모든 part, source RID, reply token 교체와 reply를
  검증한다. 기존 `:61` 테스트의 blocking/DONT_WAIT, 1·2·3·9 part, NO_DATA 시 보존 단언도 유지했다.
- Diff 분리: `02-F-R3-2.patch` — `NativeRouterReceiveSupport.java`의 3+ part 경로,
  `Received.java`의 공통 초기화와 vector population, `internal/ContractAccess.java`의 bridge,
  `RouterReceiveStorageContractTest.java`.
- Gate 결과: 저장소 회귀 2건을 포함한 7건 × 5회 모두 통과. 전체 gate는 아래 공통 결과 참조.
- BLOCKERS: 없음.

## F-R3-12 — NO_DATA 이후 재제출

- 소유 계층: Java의 기존 socket completion owner. drain 순서는 Core 계약을 따른다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:986`의 part send, `:1074`의 REQUEST DONTWAIT,
  `:1177`의 completion drain; `bindings/doc/spec/async-execution-model.ko.md` §4·§5와
  `bindings/doc/spec/async-coroutine-policy.ko.md` §4.
- 원인: `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:636`의 drain loop가
  `:1145`의 capture를 통해 inline 재제출했다.
- 수정: `captureWritable`은 기존 owner의 `retries` 목록에 준비된 entry를 모으고,
  `drainLocked`가 실제 `NO_DATA`를 받은 뒤 SEND·REQUEST를 재제출한다. 새 completion은 다음 drain의
  대상이다. 재제출로 끝난 waiter도 public wait의 settlement 목록에 넣는다. BUSY에서는 재제출하지
  않으며 runtime 실패·close에서 보관한 참조를 정리한다. poller·timer·재시도 정책은 추가하지 않았다.
- 교차언어 대조: .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:331`의
  `DrainCore`는 D-B117의 `_retries`를 NO_DATA에서 처리한다. 같은 순서로 정렬했다.
- 변경 분류: B.
- 수정 전/후 규칙 수: capture 즉시 재제출 / Core의 drain 후 재제출 **2 → 1**.
- 회귀: `CompletionDrainOrderContractTest.java:18`. SEND·REQUEST 각각에 대해
  `WRITABLE → 두 번째 queued REQUEST → NO_DATA → 재제출`의 실제 ABI 호출 순서를 검증한다.
  completion close 횟수와 public settlement도 확인한다.
- Diff 분리: `06-F-R3-12.patch` — Owner의 retry 목록, NO_DATA 처리, 실패·close 정리,
  `captureWritable`의 재제출 예약 및 회귀 클래스.
- Gate 결과: 신규 회귀 포함 7건 × 5회 모두 통과. 전체 gate는 아래 공통 결과 참조.
- BLOCKERS: 없음.

## F-R3-13 — token 없는 BACKPRESSURED의 오류 보존

- 소유 계층: Core submit result·errno. Java는 해당 typed submit exception을 만든다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:964`의 SNDTIMEO,
  `:1060`의 REQUEST slot 포화, §6 completion·submit 결과 표;
  `bindings/doc/spec/README.ko.md`의 Submit 결과 투영과 async-execution-model §5.
- 원인: `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:915`의
  `submitFailure`가 EAGAIN·ID 0을 INTERNAL_ERROR로 바꿨다. 초기 제출, retained 재제출,
  blocking SEND·reply가 이 함수를 공유했다.
- 수정: Core result·errno로 오류를 만든다. nonzero token이 있는 BACKPRESSURED만 WRITABLE을
  기다린다. 의미가 같아진 REQUEST 전용 success 검사도 기존 공통 검사로 통합했다.
- 교차언어 대조: Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:336`은 token 없는
  REQUEST backpressure를 `SubmitResult.Backpressured`와 native errno로 전달한다.
- 변경 분류: B.
- 수정 전/후 규칙 수: Core 결과 / Java tokenless 재분류 **2 → 1**.
- 회귀: `TokenlessBackpressureContractTest.java:16`. SEND·REQUEST의 async·blocking 초기 경로,
  reply 공통 경로와 SEND·REQUEST 재제출에서 BACKPRESSURED·EAGAIN 보존을 검증한다.
  실패한 초기 제출의 managed 입력도 보존된다. 실제 slot 65,536개를 채우는 대신 ABI 결과를 통제한다.
- Diff 분리: `03-F-R3-13.patch` — `submitFailure`, REQUEST success 검사 통합과 회귀 클래스.
- Gate 결과: 신규 회귀 포함 7건 × 5회 모두 통과. 전체 gate는 아래 공통 결과 참조.
- BLOCKERS: 없음.

## F-R3-14 — targetRemoved 상태와 재분류 제거

- 소유 계층: Core가 대기 토큰의 terminal과 새 제출의 결과를 구분한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:1008`의 명시적 제거,
  `:1083`의 route 없는 REQUEST, `:1155`의 completion 표; bindings async-execution-model §5.
- 원인: `bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketCore.java:106`의 pending 전체
  순회와 `CompletionOwner.java:272`, `:826`, `:859`, `:1046`, `:1125`의 `targetRemoved` 기록·재분류.
- 수정: `disconnectRid`는 Core 결과를 전달한다. pending target 제거 flag·순회·동기화·오류 override를
  삭제하고, terminal은 native errno로, 재제출 실패는 native submit result·errno로 전달한다.
- 교차언어 대조: Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:636`은
  재제출 결과를 `submitError(result.result, result.nativeErrno, ...)`로 전달한다.
- 변경 분류: B.
- 수정 전/후 규칙 수: Core 결과 / Java 제거 이력 판정 **2 → 1**.
- 회귀: `TargetRemovalResultContractTest.java:15`. SEND·REQUEST 모두 explicit removal terminal의
  ENOENT→NOT_FOUND와 이미 queued WRITABLE 뒤 제거→재제출 NOT_CONNECTED·EHOSTUNREACH를 검증한다.
- 기존 integration fixture: `AsyncSubmitTypedResultContractTest.java:136`은 HWM의 일시적 포화를
  미발행 토큰의 보장으로 사용했다. TCP I/O가 credit을 회복해 WRITABLE을 먼저 발행하면
  NOT_CONNECTED가 맞으므로 그 상태로 NOT_FOUND를 단언할 수 없다. `peerWeight(0)`를 연결 전에
  설정하고 반대 방향 DATA 수신으로 RID를 확인해 제거 전까지 wake하지 않는 토큰을 만든다.
  NOT_FOUND 단언은 그대로이며 미완료와 source RID 단언을 추가했다. weight 의미는 Core socket
  `07-router.ko.md:129` 및 공통 part send의 weight 0 계약을 따른다. HWM capacity 회귀는 유지했다.
- Diff 분리: `04-F-R3-14.patch` — `SocketCore.java`, Owner의 제거 이력 관련 hunk,
  신규 회귀 클래스와 integration fixture의 제거 전제 설정.
- Gate 결과: 신규 회귀 포함 7건 × 5회 및 해당 integration 클래스 8건 × 5회 통과.
  전체 gate는 아래 공통 결과 참조.
- BLOCKERS: 없음.

## F-R3-15 — blocking admission과 drain 잠금 분리

- 소유 계층: Core가 동시 part sequence의 admission을 결정한다. Java는 native handle의 수명과
  자기 registry만 보호한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md` §2(`:44`), part sequence(`:940`);
  `bindings/doc/spec/README.ko.md:1347`의 binding 송신 직렬화 금지,
  async-execution-model §5의 완료 합류 계약.
- 원인: `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:248`의
  `withSendSequenceLock`이 blocking native 호출 동안 `drainLock`을 보유했다. SEND·reply와
  `SocketSendPlane`의 blocking 호출들도 같은 잠금을 사용했다.
- 수정: blocking SEND·reply는 `submitPartsAttempt`를 직접 사용한다. 기존 `nativeCallGate`의
  공유 read lock을 `withNativeCall`에서 재사용하고 `SocketSendPlane`도 여기에 연결한다.
  sender끼리 상호 배제하지 않으며 drain도 공유 read lock으로 진행한다. DONTWAIT의 짧은
  registry publication 동기화와 close의 native 수명 보호는 유지한다.
- 교차언어 대조: Rust `bindings/rust/src/internal/completion_owner.rs:350`의 `with_submit`은
  공유 lifecycle read guard로 concurrent submit을 허용한다. C++
  `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:145`의 native part 제출도 Java의
  전체 sequence 상호 배제에 해당하는 잠금을 두지 않는다.
- 변경 분류: B.
- 수정 전/후 규칙 수: Java 범위에서 Core admission / Java 송신 직렬화 **2 → 1**.
- 회귀: `ConcurrentAdmissionContractTest.java:15`. 첫 blocking SEND가 native admission에 진입한
  상태에서 latch를 유지하고, 다른 thread의 SEND와 queued REQUEST drain이 먼저 끝나는지 검증한다.
  첫 SEND의 해제 전에 진행을 확인하므로 sleep으로 실행 순서를 추정하지 않는다.
- Diff 분리: `05-F-R3-15.patch` — Owner의 blocking SEND·reply, 공유 native-call guard,
  `SocketSendPlane.java`의 기존 호출부와 회귀 클래스.
- Gate 결과: 신규 회귀 포함 7건 × 5회 모두 통과. 전체 gate는 아래 공통 결과 참조.
- BLOCKERS: 없음.

## 설계 대안과 diff 적용

- F-R3-2: 모든 part 수를 임시 vector로 통합하면 1·2 part에서도 추가 준비 할당이 생긴다.
  기존 scalar 입력과 완성된 vector 입력은 유지하고, 저장소 초기화의 소유자를 하나로 모았다.
- F-R3-12: drain-local 목록은 BUSY로 종료된 호출에서 아직 재제출하지 못한 entry 처리가 별도로
  필요하다. .NET처럼 기존 owner가 목록을 보유해 NO_DATA라는 한 경계에서 처리한다.
- F-R3-15: 별도 exclusive send lock은 drain만 진행시킬 뿐 다른 sender는 계속 직렬화한다.
  기존 공유 native 수명 guard를 사용해 Core가 모든 동시 submit의 결과를 정하도록 했다.

원인별 patch는 `/tmp/zlink-java-r3-patches/`에 있다. 적용 순서는
`01-F-R3-1.patch` → `02-F-R3-2.patch` → `03-F-R3-13.patch` → `04-F-R3-14.patch` →
`05-F-R3-15.patch` → `06-F-R3-12.patch`다. 첫 patch에 추가한 테스트 fixture는 후속 completion
회귀가 공유한다. 임시 source tree에 각 patch를 적용한 뒤 모든 최종 Java 파일이 현재 작업 파일과
byte 단위로 같은지 검증했다. `CompletionOwner.java`의 인접한 hunk도 원인별로 분리했다.

`CompletionNativeFixture`는 별도 probe JVM에서 native symbol lookup만 교체한다. 통제하지 않은
함수와 message ownership은 지정한 실제 Core를 사용한다. production hook·poller·timer는 추가하지
않았다. Linux의 native errno 저장소를 사용하며 이번 gate는 Linux x86_64·JDK 22.0.2에서 실행했다.

## 검증 결과

- 수정 전 코드 대조: 새 회귀 6건 모두 예상 원인으로 실패, 기존 저장소 회귀 1건 통과.
  `/tmp/zlink-java-r3-baseline.log`, `/tmp/zlink-java-r3-baseline-results/`에 보존했다.
- 수정 후 회귀: 신규 6건 + 기존 저장소 1건 = 7건 × 5회, 실패·skip 0.
  `/tmp/zlink-java-r3-repeat-{1..5}.log`와 각 `-results/`에 보존했다.
- 제거 fixture: 해당 integration 클래스의 INPROC·TCP 8건 × 5회, 실패 0.
  `/tmp/zlink-java-r3-removal-fixture.log`, `/tmp/zlink-java-r3-removal-fixture-{2..5}.log`.
- 첫 전체 gate: integration TCP 제거 경합 1건 및 samples 2건 실패. integration은 위 F-R3-14 fixture
  전제를 수정했다. samples `runStreamPacketCallback`, `runMonitorRecv`는 각 30초 제한 안에 실행
  task까지 도달하지 못했다. 당시 load average는 약 66, CPU는 20개였다. 시간 제한을 변경하지 않았다.
  `/tmp/zlink-java-r3-gate.log`, `/tmp/zlink-java-r3-gate-initial-integration-results/`,
  `/tmp/zlink-java-r3-gate-initial-samples.log`에 첫 결과를 보존했다.

최종 gate 명령:

```bash
ZLINK_CORE_SOURCE=local \
ZLINK_CORE_INCLUDE_DIR=/home/hep7/project/zlink/core/include \
ZLINK_CORE_LIB_DIR=/home/hep7/project/zlink/core/build-dev/lib \
flock /tmp/zlink-samples-gate.lock bash bindings/java/tests/run_tests.sh
```

최종 gate는 exit 0으로 완료했다. 모든 task와 sample smoke의 실패는 0건이다.

| Task | 테스트 수 | 실패 | Skip |
|---|---:|---:|---:|
| `:test` | 115 | 0 | 1 |
| `:integrationTest` | 25 | 0 | 0 |
| `:zlink-ext-netty:test` | 3 | 0 | 0 |
| `:kotlin-contract-test:test` | 4 | 0 | 0 |
| Samples | 7 | 0 | 0 |

기존 skip 1건은 `MonitorConnectionIdentityContractTest.inprocClosedKeepsReadyConnectionIdentity`다.
이번 작업에서 skip이나 기존 assertion을 낮추지 않았다. 최종 로그는
`/tmp/zlink-java-r3-gate-final.log`와 `bindings/java/build/test-runner-logs/`에 있으며,
JUnit 결과는 각 task의 `build/test-results/`에 있다.

Core library와 Java가 준비한 native resource의 SHA-256은 모두
`64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`이다.
`git diff --check -- bindings/java`를 통과했다. perf benchmark는 실행하지 않았다.

## BLOCKERS

없음. 첫 gate의 실패는 최종 gate에서 남지 않았다.
