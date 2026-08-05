# Java 공통 runtime 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Host relocation과 종료 계약](../../../../28-graceful-drain-handoff.ko.md)

이 문서는 Java에서 host의 실행 상태, object relocation, 종료 요청과 공통 비동기 operation을 표현하는 공개 타입을
고정한다. 공통 문서가 동작을 정의하며, 아래 선언은 Java에서 사용하는 타입과 member의 정확한 형태를
보여 준다.

공개 계약은 `systems.zlink.framework` Java module이 소유한다. 이 module은 이 exact interface에 기록한
application package와 `runtime.host`만 일반 consumer에게 export한다. Raw binding 구현은 별도 internal
artifact에 있으며 application compile classpath에는 포함되지 않는다. Named module에서도 Framework
companion module에 필요한 package만 export한다. 따라서 raw binding type은 public API inventory에
포함되지 않는다.

```java
public enum ZLinkFrameworkRuntimeState {
    PREPARING(0), SERVING(1), RELOCATING(2), RELOCATED(3),
    DRAINING(4), STOPPED(5), ERROR(6);
    private final int wireValue;
    ZLinkFrameworkRuntimeState(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public enum ZLinkFrameworkRelocationOutcome {
    RELOCATED(0), BLOCKED(1);
    private final int wireValue;
    ZLinkFrameworkRelocationOutcome(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public enum ZLinkFrameworkRelocationMode {
    PLANNED_MAINTENANCE(0), ROLLING_UPDATE(1);
    private final int wireValue;
    ZLinkFrameworkRelocationMode(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public enum ZLinkFrameworkRelocationReason {
    NONE(0), TARGET_UNAVAILABLE(1), STORE_UNAVAILABLE(2),
    RELOCATION_DISABLED(3), STATE_INCOMPATIBLE(4),
    DEADLINE_EXCEEDED(5), RELOCATION_FAILED(6),
    RUNTIME_NOT_READY(7), MANUAL_TOPOLOGY_UNSUPPORTED(8),
    SHUTDOWN_REQUESTED(9), OPERATION_IN_PROGRESS(10);
    private final int wireValue;
    ZLinkFrameworkRelocationReason(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public record ZLinkFrameworkRelocationOptions(
    ZLinkFrameworkRelocationMode mode,
    Long targetApplicationVersion,
    Duration deadline) {}

public record ZLinkFrameworkRelocationResult(
    ZLinkFrameworkRelocationMode mode,
    long effectiveTargetApplicationVersion,
    ZLinkFrameworkRelocationOutcome outcome,
    ZLinkFrameworkRelocationReason reason) {}

public enum ZLinkFrameworkTerminationOutcome {
    STOPPED(0), FORCE_STOPPED(1);
    private final int wireValue;
    ZLinkFrameworkTerminationOutcome(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public enum ZLinkFrameworkTerminationReason {
    NONE(0), DEADLINE_EXCEEDED(1), TEARDOWN_FAILED(2);
    private final int wireValue;
    ZLinkFrameworkTerminationReason(int wireValue) { this.wireValue = wireValue; }
    public int wireValue() { return wireValue; }
}

public record ZLinkFrameworkTerminationResult(
    ZLinkFrameworkTerminationOutcome outcome,
    ZLinkFrameworkTerminationReason reason) {}

public final class ZLinkFrameworkRuntime
    implements AutoCloseable, ZLinkMessageFlowControl {
    public ZLinkClient client();
    public void setMessageFlowMode(ZLinkMessageFlowLogMode mode);
    public ZLinkMessageFlowLogMode messageFlowMode();
    public ZLinkFanoutClient fanout();
    public ZLinkRouteClient route();
    public ZLinkRouteMeshRuntime routeMeshRuntime();
    public ZLinkClientServerRuntime clientServerRuntime();
    public ZLinkFanoutRuntime fanoutRuntime();
    public ZLinkSpotManager spotManager();
    public ZLinkSpotOutbound spotOutbound();
    public ZLinkSpotPublisherClient spotPublisherClient();
    public ZLinkLocationRuntimeQuery monitoringLocationRuntimeQuery();
    public ZLinkLocationReadiness locationReadiness();
    public boolean stopSpotRuntime();
    public ZLinkActorManager actorManager();
    public ZLinkActorClient actorClient();
    public ZLinkSessionActorsRuntime sessionActors(String streamNodeName, RoutingId sessionRid);

    public ZLinkFrameworkRuntimeStatus status();
    public Flow.Publisher<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> observe();
    public CompletionStage<ZLinkFrameworkRelocationResult> relocate(
        ZLinkFrameworkRelocationOptions options);
    public CompletionStage<ZLinkFrameworkTerminationResult> shutdown();
    public CompletionStage<ZLinkFrameworkTerminationResult> shutdown(Duration deadline);
    public void close();
}
```

`observe()`가 전달하는 `ZLinkObservedStatus<T>`와 `ZLinkObservationLoss`의 canonical declaration은
[Monitoring 공개 인터페이스](monitoring.ko.md)가 소유한다. 이 문서는 host status stream이 같은 envelope를
사용한다는 사실만 고정한다.

`relocate(options)`는 신규 application admission과 placement를 닫고 현재 object를 compatible target으로
이전한다. 성공하면 `RELOCATED` 상태가 되며 host process와 infrastructure connection은 유지한다. User Spot은 [Spot](../../../../01-glossary.ko.md#spot)과 current
member Actor 전체를 하나의 aggregate로 옮긴다. Participant 총수에 고정 상한을 두지 않는다.
Aggregate participant 하나라도 `disableRelocation()`을 선택했으면
`Blocked/RelocationDisabled`, target·capacity·reservation을 확보할 수 없으면 `Blocked/TargetUnavailable`,
application version·type·state 보존 adapter capability가 맞지 않으면 `Blocked/StateIncompatible`로 끝난다. 이
preflight failure는 admission을 변경하지 않는다. [User Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot)이 존재한다는 사실만으로 relocation을 차단하지 않는다.
Local manual RouteMesh peer, ClientServer client endpoint, fanout subscriber endpoint 또는 manual fanout publisher가
하나라도 있으면 `Blocked/ManualTopologyUnsupported`로 끝난다. Automatic RouteMesh는 source의 Core peer table에서
descriptor와 같은 RID·lifecycle generation이 admitted·ready가 된 뒤에만 `RELOCATING`으로 전환한다.
`shutdown()`은 새 relocation을 시작하지 않는다. 두 operation 모두
숨은 remote `GetOrCreate`를 수행하지 않으며, waiter cancellation은 이미 시작한 shared operation을
취소하지 않는다. 각 호출은 shared operation 결과를 따르는 전용 `CompletableFuture` view를 반환한다.
`toCompletableFuture().cancel(...)`은 그 waiter만 해제하며 host operation은 계속 진행되고 다른 waiter는 같은
terminal 결과를 받는다. 별도 public cancellation token이나 host operation 취소 member를 추가하지 않는다.
`RELOCATING`에서 `shutdown()`을 호출하면 실행 중인 atomic relocation unit만 terminal 상태까지 확정하고
나머지 relocation을 시작하지 않는다. Relocation waiter는 `Blocked/ShutdownRequested`를 받고 host는
bounded cleanup을 계속한다.

### Relocation mode와 target 선택

호출자는 mode를 항상 명시한다. `targetApplicationVersion`과 `deadline`은 nullable component다.
`deadline == null`이면 Framework의 host relocation 기본 deadline을 사용한다.

- `PLANNED_MAINTENANCE`는 같은 application version을 유지하는 node 점검이나 재부팅에 사용한다.
  `targetApplicationVersion`은 반드시 `null`이어야 하며, 결과의
  `effectiveTargetApplicationVersion`은 source host의 application version이다.
- `ROLLING_UPDATE`는 새 application version으로 교체할 때 사용한다.
  `targetApplicationVersion`은 반드시 지정해야 하고 source version보다 커야 한다. Framework는 이 값과
  application version이 정확히 같은 node만 target 후보로 사용하며, 중간 version이나 그보다 높은 다른
  version으로 대체하지 않는다.

Mode와 target version 조합이 위 조건에 맞지 않으면 Framework는 admission이나 placement 상태를 변경하지
않고 `IllegalArgumentException`으로 호출을 거부한다. 유효한 호출의 후보 선택 순서는 다음과 같다.

1. `PLANNED_MAINTENANCE`는 source version과 같은 node만, `ROLLING_UPDATE`는 지정한 target version과
   정확히 같은 node만 남긴다.
2. 같은 Mesh에서 source가 아니며 `SERVING` 상태인 Object Server만 남긴다.
3. stable type, factory에서 선택한 relocation 동작과 adapter capability가 맞는지 확인한다.
4. population capacity와 reservation 가능 여부를 확인하고 source와 같은 maintenance wave를 제외한다.
5. 같은 descriptor snapshot에서 RID와 lifecycle generation이 일치하는 Core peer가 `ADMITTED`인
   node만 남긴다.
6. 남은 후보에 node-wide placement weight를 적용한다.

Version 조건을 먼저 적용하므로 capability나 capacity가 충분하더라도 다른 version node로 fallback하지
않는다. Version·wave·capacity 또는 exact-ready target이 없으면 deadline까지 다시 확인한 뒤
`Blocked/TargetUnavailable`이다. Stable type, factory, relocation policy 또는 adapter가 맞지 않으면
`Blocked/StateIncompatible`이다. Store 조회 실패는 `Blocked/StoreUnavailable`이다.

같은 shared relocation이 실행 중일 때 mode와 effective target version이 같은 호출은 기존 operation에
참여하고 같은 terminal result를 받는다. 첫 호출의 deadline이 shared operation deadline을 고정하며 뒤에
참여한 호출의 deadline은 operation을 연장하거나 단축하지 않는다. 실행 중인 operation과 mode 또는 target
version이 다른 호출은 현재 operation을 변경하거나 대기열에 넣지 않고
`Blocked/OperationInProgress`를 반환한다. 이 결과에는 거부된 호출이 요청한 mode와, planned
maintenance이면 source version, rolling update이면 요청한 target version을
`effectiveTargetApplicationVersion`으로 기록한다.

모든 target을 `Prepared`로 만들고 relocation commit을 publish하기 전에 deadline이 먼저 끝나면 relocation
reference와 reservation을 durable abort 순서로 정리하고 source authority와 admission을 복원한 뒤
`Blocked/DeadlineExceeded`를 반환한다. Commit 뒤에는 source로 rollback하지 않으며 같은 target
process가 실행 중일 때만 남은 단계를 처리한다. Target process가 종료되면
다른 runtime이 relocation을 자동으로 이어받지 않으며 `RELOCATED`를 반환하지 않는다.

`ZLinkFrameworkRuntime`은 RouteMesh, ClientServer와 automatic fanout의 monitoring view를 각각 하나씩
소유한다. 세 accessor는 runtime 수명 동안 같은 객체를 반환하며 호출할 때 새 adapter를 만들지 않는다.
Spring starter가 제공하는 topology runtime bean도 이 accessor가 반환한 객체와 reference identity가 같다.

Spring starter는 `ZLinkFrameworkRuntime` bean을 제공한다. 운영 maintenance endpoint는
`relocate(options)` 결과가 `RELOCATED`일 때 `shutdown()`을 호출한다. Relocation 없이 종료하려면
`shutdown()`만 호출한다. 별도 drain facade와 MeshName을 받는 partial operation은 없다.

Application은 `ZLinkFrameworkRuntime`을 직접 시작하지 않는다. Runtime 생성과 시작은 Spring starter가
소유한다. 따라서 `start(...)` factory는 public contract에 포함하지 않는다. Core의 internal bootstrap은
starter module에만 qualified export한다. Testkit은 test source에만 같은 package access helper를 둔다.

## Exact public member `javap` inventory

아래 선언은 `javap`가 출력하는 binary signature 형식으로 Java public type과 member를 고정한다.

```java
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState PREPARING;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState SERVING;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState RELOCATING;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState RELOCATED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState DRAINING;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState STOPPED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState ERROR;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome RELOCATED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome BLOCKED;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode PLANNED_MAINTENANCE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode ROLLING_UPDATE;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason NONE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason TARGET_UNAVAILABLE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason STORE_UNAVAILABLE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason RELOCATION_DISABLED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason STATE_INCOMPATIBLE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason DEADLINE_EXCEEDED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason RELOCATION_FAILED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason RUNTIME_NOT_READY;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason MANUAL_TOPOLOGY_UNSUPPORTED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason SHUTDOWN_REQUESTED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason OPERATION_IN_PROGRESS;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions extends java.lang.Record {
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions(systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode, java.lang.Long, java.time.Duration);
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode mode();
  public java.lang.Long targetApplicationVersion();
  public java.time.Duration deadline();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult extends java.lang.Record {
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult(systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode, long, systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome, systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason);
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode mode();
  public long effectiveTargetApplicationVersion();
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome outcome();
  public systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason reason();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome STOPPED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome FORCE_STOPPED;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason extends java.lang.Enum<systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason> {
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason NONE;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason DEADLINE_EXCEEDED;
  public static final systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason TEARDOWN_FAILED;
  public int wireValue();
}
public final class systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult extends java.lang.Record {
  public systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationResult(systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome, systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason);
  public systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome outcome();
  public systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationReason reason();
}
public interface systems.zlink.framework.ZLinkMessageContext {
  public abstract java.util.Optional<java.lang.String> meshName();
  public abstract java.util.Optional<java.lang.String> channelName();
  public abstract java.lang.String packetName();
  public abstract java.util.Optional<java.lang.String> contentType();
  public abstract java.util.Map<java.lang.String, java.lang.String> metadata();
  public abstract java.util.Optional<java.lang.String> correlationId();
}
public final class systems.zlink.framework.ZLinkHandlerDispatchKind extends java.lang.Enum<systems.zlink.framework.ZLinkHandlerDispatchKind> {
  public static final systems.zlink.framework.ZLinkHandlerDispatchKind NODE_DIRECT_SEND;
  public static final systems.zlink.framework.ZLinkHandlerDispatchKind NODE_DIRECT_REQUEST;
  public static final systems.zlink.framework.ZLinkHandlerDispatchKind CHANNEL_SEND;
  public static final systems.zlink.framework.ZLinkHandlerDispatchKind CHANNEL_REQUEST;
  public static final systems.zlink.framework.ZLinkHandlerDispatchKind CLASSIC_FANOUT;
}
public interface systems.zlink.framework.ZLinkHandlerFilterContext extends systems.zlink.framework.ZLinkMessageContext {
  public abstract systems.zlink.framework.ZLinkHandlerDispatchKind dispatchKind();
}
public interface systems.zlink.framework.ZLinkHandlerFilter {
  public abstract <T> java.util.concurrent.CompletionStage<T> invoke(systems.zlink.framework.ZLinkHandlerFilterContext, systems.zlink.framework.ZLinkHandlerFilterNext<T>);
}
public interface systems.zlink.framework.ZLinkMessageSerializer {
  public abstract <T> systems.zlink.framework.ZLinkEncodedPayload serialize(T);
  public abstract <T> T deserialize(systems.zlink.framework.ZLinkEncodedPayload, java.lang.Class<T>);
  public default void prepare(java.lang.Class<?>);
}
public interface systems.zlink.framework.ZLinkHandlerFilterNext<T> {
  public abstract java.util.concurrent.CompletionStage<T> invoke();
}
public final class systems.zlink.framework.errors.ZLinkFrameworkErrorKind extends java.lang.Enum<systems.zlink.framework.errors.ZLinkFrameworkErrorKind> {
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind NOT_FOUND;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind ALREADY_EXISTS;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind TYPE_MISMATCH;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind NOT_CONFIGURED;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind REJECTED;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind UNAVAILABLE;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind CAPACITY_EXCEEDED;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind DEADLINE_EXCEEDED;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind SHUTTING_DOWN;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind PROTOCOL_ERROR;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind INVALID_OPERATION;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind DATA_LOST;
  public static final systems.zlink.framework.errors.ZLinkFrameworkErrorKind INTERNAL_FAILURE;
  public static systems.zlink.framework.errors.ZLinkFrameworkErrorKind[] values();
  public static systems.zlink.framework.errors.ZLinkFrameworkErrorKind valueOf(java.lang.String);
  public int value();
  public static systems.zlink.framework.errors.ZLinkFrameworkErrorKind fromValue(int);
}
public class systems.zlink.framework.errors.ZLinkFrameworkException extends java.lang.RuntimeException {
  public systems.zlink.framework.errors.ZLinkFrameworkException(systems.zlink.framework.errors.ZLinkFrameworkErrorKind, java.lang.String);
  public systems.zlink.framework.errors.ZLinkFrameworkException(systems.zlink.framework.errors.ZLinkFrameworkErrorKind, java.lang.String, java.lang.Throwable);
  public systems.zlink.framework.errors.ZLinkFrameworkErrorKind kind();
}
```

`ZLinkFrameworkErrorKind.value()`는 선언 순서와 무관하게 공통 숫자 `0..12`를 반환한다.
`fromValue(int)`도 [공통 오류 모델](../../../../32-framework-error-model.ko.md)의 같은 mapping을 사용한다.
Public exception은 재시도 여부를 제공하지 않는다.

## Serializer와 오류 public signature

```java
public final class systems.zlink.framework.ZLinkEncodedPayload {
  public static systems.zlink.framework.ZLinkEncodedPayload from(byte[]);
  public byte[] bytes();
}
public final class systems.zlink.framework.errors.ZLinkConfigurationException extends systems.zlink.framework.errors.ZLinkFrameworkException {
  public systems.zlink.framework.errors.ZLinkConfigurationException(java.lang.String);
  public systems.zlink.framework.errors.ZLinkConfigurationException(java.lang.String, java.lang.Throwable);
}
public final class systems.zlink.framework.messaging.ZLinkMessage {
  public static systems.zlink.framework.messaging.ZLinkMessage empty();
  public static systems.zlink.framework.messaging.ZLinkMessage of(java.lang.Object);
  public static systems.zlink.framework.messaging.ZLinkMessage fromEncoded(systems.zlink.framework.ZLinkEncodedPayload, systems.zlink.framework.ZLinkMessageSerializer);
  public boolean isEmpty();
  public <T> T decode(java.lang.Class<T>);
  public systems.zlink.framework.ZLinkEncodedPayload toEncodedPayload(systems.zlink.framework.ZLinkMessageSerializer);
}
public final class systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec implements systems.zlink.framework.configuration.ZLinkCodecExtension,systems.zlink.stream.connector.ZLinkStreamTypedCodec {
  public static systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec defaultCodec();
  public static systems.zlink.framework.codecs.msgpack.ZLinkMessagePackCodec forPayloadTypes(java.util.function.Predicate<java.lang.Class<?>>);
  public <T> systems.zlink.stream.connector.ZLinkStreamEncodedPayload encode(java.lang.String, T);
  public <T> T decode(systems.zlink.stream.connector.ZLinkStreamEncodedPayload, java.lang.Class<T>);
  public void register(systems.zlink.framework.configuration.ZLinkCodecRegistrar);
}
public final class systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec implements systems.zlink.framework.configuration.ZLinkCodecExtension,systems.zlink.stream.connector.ZLinkStreamTypedCodec {
  public static systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec defaultCodec();
  public <T> systems.zlink.stream.connector.ZLinkStreamEncodedPayload encode(java.lang.String, T);
  public <T> T decode(systems.zlink.stream.connector.ZLinkStreamEncodedPayload, java.lang.Class<T>);
  public void register(systems.zlink.framework.configuration.ZLinkCodecRegistrar);
}
```
