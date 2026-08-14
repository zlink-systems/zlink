package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorBindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorUnbindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkStreamPhysicalSubmitOwnershipTest {
    private static final RoutingId SESSION = RoutingId.from("session");
    private static final RoutingId NODE_A = RoutingId.from("actor-node-a");
    private static final RoutingId NODE_B = RoutingId.from("actor-node-b");
    private static final long BINDING_GENERATION = 6_001;

    @Test
    void storedBindingRelayDelegatesTheAcceptedSequenceOnceToBackendAsync() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicInteger syncSubmits = new AtomicInteger();
        AtomicReference<Long> submittedSequence = new AtomicReference<>();
        AtomicReference<String> submittedPayload = new AtomicReference<>();
        ZLinkBackendStreamSocket stream = streamProxy((method, arguments) -> {
            if (method.getName().equals("relayBoundActorAsync")
                && arguments.length == 5) {
                asyncSubmits.incrementAndGet();
                submittedSequence.set((Long) arguments[2]);
                @SuppressWarnings("unchecked")
                List<Message> parts = (List<Message>) arguments[4];
                submittedPayload.set(parts.getFirst().toUtf8String());
                return physicalTerminal;
            }
            if (method.getName().equals("relayBoundActor")) {
                syncSubmits.incrementAndGet();
                throw new IllegalStateException(
                    "Framework must not retry a physical relay submit");
            }
            throw new UnsupportedOperationException(method.getName());
        });
        ZLinkSessionRelayHeaders relayHeaders = new ZLinkSessionRelayHeaders();
        ZLinkBoundActor actor = new ZLinkBoundActor(
            stream,
            SESSION,
            new ZLinkBackendActorRef(NODE_A, "actor-1", 7),
            "game",
            Optional.empty(),
            null,
            new RawSerializer(),
            0,
            BINDING_GENERATION,
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.JSON,
            relayHeaders,
            null,
            () -> true,
            operation -> operation.apply(73),
            ZLinkRelayMetadataPolicy.EMPTY);
        relayHeaders.enter(oneWayHeader("Play"));
        try {
            CompletionStage<Void> submission = actor.relay(
                ZLinkMessage.of("payload"));

            assertEquals(1, asyncSubmits.get());
            assertEquals(0, syncSubmits.get());
            assertEquals(73L, submittedSequence.get());
            assertEquals("payload", submittedPayload.get());
            assertFalse(submission.toCompletableFuture().isDone());
            physicalTerminal.complete(null);
            submission.toCompletableFuture().join();
        } finally {
            relayHeaders.exit();
        }
    }

    @Test
    void storedBindingRelayCancellationCancelsTheBackendPhysicalTerminal() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        ZLinkBackendStreamSocket stream = streamProxy((method, arguments) -> {
            if (method.getName().equals("relayBoundActorAsync")
                && arguments.length == 5) {
                return physicalTerminal;
            }
            if (method.getName().equals("relayBoundActor")) {
                throw new IllegalStateException(
                    "Framework must not retry a physical relay submit");
            }
            throw new UnsupportedOperationException(method.getName());
        });
        ZLinkSessionRelayHeaders relayHeaders = new ZLinkSessionRelayHeaders();
        ZLinkBoundActor actor = new ZLinkBoundActor(
            stream,
            SESSION,
            new ZLinkBackendActorRef(NODE_A, "actor-1", 7),
            "game",
            Optional.empty(),
            null,
            new RawSerializer(),
            0,
            BINDING_GENERATION,
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.JSON,
            relayHeaders,
            null,
            () -> true,
            operation -> operation.apply(74),
            ZLinkRelayMetadataPolicy.EMPTY);
        relayHeaders.enter(oneWayHeader("Play"));
        try {
            CompletableFuture<Void> submission = actor.relay(
                ZLinkMessage.of("payload")).toCompletableFuture();

            assertTrue(submission.cancel(false));
            assertTrue(physicalTerminal.isCancelled());
        } finally {
            relayHeaders.exit();
        }
    }

    @Test
    void productionStoredBindingRelayCancellationCancelsThePhysicalTerminal() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicInteger syncSubmits = new AtomicInteger();
        ZLinkBackendStreamSocket stream = streamProxy(
            (method, arguments) -> switch (method.getName()) {
                case "boundActorBindingGeneration" -> BINDING_GENERATION;
                case "allocateBoundSessionIngressSequence" -> 0L;
                case "bindActor" ->
                    (ZLinkBackendActorBindOperation) timeout ->
                        CompletableFuture.completedFuture(null);
                case "requestBoundActor" ->
                    CompletableFuture.completedFuture(List.of());
                case "relayBoundActorAsync" -> {
                    asyncSubmits.incrementAndGet();
                    yield physicalTerminal;
                }
                case "relayBoundActor" -> {
                    syncSubmits.incrementAndGet();
                    throw new IllegalStateException(
                        "Framework must not retry a physical relay submit");
                }
                default -> defaultValue(method.getReturnType());
            });
        ZLinkSessionActor actor = runtime(stream)
            .bind(new ActorRef("actor-1", 7, "game", NODE_A))
            .toCompletableFuture()
            .join();
        ZLinkSessionActorsRuntime.enterRelayDispatch(oneWayHeader("Play"));
        try {
            CompletableFuture<Void> submission = actor.relay(
                ZLinkMessage.of("payload")).toCompletableFuture();

            assertEquals(1, asyncSubmits.get());
            assertEquals(0, syncSubmits.get());
            assertTrue(submission.cancel(false));
            assertTrue(physicalTerminal.isCancelled());
        } finally {
            ZLinkSessionActorsRuntime.exitRelayDispatch();
        }
    }

    @Test
    void storedBindingRelayPropagatesOneBackendPhysicalErrorWithoutRetry() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicInteger syncSubmits = new AtomicInteger();
        ZLinkBackendStreamSocket stream = streamProxy((method, arguments) -> {
            if (method.getName().equals("relayBoundActorAsync")
                && arguments.length == 5) {
                asyncSubmits.incrementAndGet();
                return physicalTerminal;
            }
            if (method.getName().equals("relayBoundActor")) {
                syncSubmits.incrementAndGet();
                return false;
            }
            throw new UnsupportedOperationException(method.getName());
        });
        ZLinkSessionRelayHeaders relayHeaders = new ZLinkSessionRelayHeaders();
        ZLinkBoundActor actor = new ZLinkBoundActor(
            stream,
            SESSION,
            new ZLinkBackendActorRef(NODE_A, "actor-1", 7),
            "game",
            Optional.empty(),
            null,
            new RawSerializer(),
            0,
            BINDING_GENERATION,
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.JSON,
            relayHeaders,
            null,
            () -> true,
            operation -> operation.apply(75),
            ZLinkRelayMetadataPolicy.EMPTY);
        relayHeaders.enter(oneWayHeader("Play"));
        try {
            CompletionStage<Void> submission = actor.relay(
                ZLinkMessage.of("payload"));
            IllegalStateException rejected =
                new IllegalStateException("physical submit failed");

            physicalTerminal.completeExceptionally(rejected);

            CompletionException failure = assertThrows(
                CompletionException.class,
                () -> submission.toCompletableFuture().join());
            assertSame(rejected, failure.getCause());
            assertEquals(1, asyncSubmits.get());
            assertEquals(0, syncSubmits.get());
        } finally {
            relayHeaders.exit();
        }
    }

    @Test
    void targetOutboundFifoWaitsForOneBackendPhysicalTerminalAtATime() {
        CompletableFuture<Void> firstTerminal = new CompletableFuture<>();
        CompletableFuture<Void> secondTerminal = new CompletableFuture<>();
        ArrayDeque<CompletableFuture<Void>> terminals = new ArrayDeque<>(
            List.of(firstTerminal, secondTerminal));
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicInteger syncSubmits = new AtomicInteger();
        ZLinkBackendStreamSocket stream = relocationStream(
            asyncSubmits, syncSubmits, terminals);
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        runtime.bind(new ActorRef("actor-1", 7, "game", NODE_A))
            .toCompletableFuture().join();
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation = relocation();
        runtime.applyRelocationSealCommand(seal(relocation))
            .toCompletableFuture().join();
        ZLinkServiceM6BWireCodec.BoundSessionSend target = targetBoundSend();
        ZLinkSessionActorsRuntime.TargetOutboundAdmission first =
            runtime.admitBoundSessionSend(
                NODE_B, 4, target, outboundPayload("first"));
        ZLinkSessionActorsRuntime.TargetOutboundAdmission second =
            runtime.admitBoundSessionSend(
                NODE_B, 4, target, outboundPayload("second"));

        runtime.applyRelocationRouteCommand(route(relocation))
            .toCompletableFuture().join();

        assertEquals(1, asyncSubmits.get());
        assertEquals(0, syncSubmits.get());
        assertFalse(first.settlement().toCompletableFuture().isDone());
        assertFalse(second.settlement().toCompletableFuture().isDone());

        firstTerminal.complete(null);
        awaitCount(asyncSubmits, 2);
        assertEquals(
            ZLinkSessionActorsRuntime.TargetOutboundSettlement.DELIVERED,
            first.settlement().toCompletableFuture().join());
        assertFalse(second.settlement().toCompletableFuture().isDone());

        secondTerminal.complete(null);
        assertEquals(
            ZLinkSessionActorsRuntime.TargetOutboundSettlement.DELIVERED,
            second.settlement().toCompletableFuture().join());
    }

    @Test
    void targetOutboundShutdownCancelsThePendingBackendPhysicalTerminal() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicInteger syncSubmits = new AtomicInteger();
        ZLinkBackendStreamSocket stream = relocationStream(
            asyncSubmits,
            syncSubmits,
            new ArrayDeque<>(List.of(physicalTerminal)));
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        runtime.bind(new ActorRef("actor-1", 7, "game", NODE_A))
            .toCompletableFuture().join();
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation = relocation();
        runtime.applyRelocationSealCommand(seal(relocation))
            .toCompletableFuture().join();
        ZLinkSessionActorsRuntime.TargetOutboundAdmission admission =
            runtime.admitBoundSessionSend(
                NODE_B, 4, targetBoundSend(), outboundPayload("pending"));
        runtime.applyRelocationRouteCommand(route(relocation))
            .toCompletableFuture().join();

        runtime.notifyDisconnectedAll(Duration.ofSeconds(1))
            .toCompletableFuture().join();

        assertEquals(1, asyncSubmits.get());
        assertEquals(0, syncSubmits.get());
        assertTrue(physicalTerminal.isCancelled());
        assertEquals(
            ZLinkSessionActorsRuntime.TargetOutboundSettlement.SHUTDOWN,
            admission.settlement().toCompletableFuture().join());
    }

    @Test
    void targetOutboundErrorSettlesOnceAndAdvancesTheOwnedFifo() {
        CompletableFuture<Void> failedTerminal = new CompletableFuture<>();
        CompletableFuture<Void> nextTerminal = new CompletableFuture<>();
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicInteger syncSubmits = new AtomicInteger();
        ZLinkBackendStreamSocket stream = relocationStream(
            asyncSubmits,
            syncSubmits,
            new ArrayDeque<>(List.of(failedTerminal, nextTerminal)));
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        runtime.bind(new ActorRef("actor-1", 7, "game", NODE_A))
            .toCompletableFuture().join();
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation = relocation();
        runtime.applyRelocationSealCommand(seal(relocation))
            .toCompletableFuture().join();
        ZLinkServiceM6BWireCodec.BoundSessionSend target = targetBoundSend();
        ZLinkSessionActorsRuntime.TargetOutboundAdmission failed =
            runtime.admitBoundSessionSend(
                NODE_B, 4, target, outboundPayload("failed"));
        ZLinkSessionActorsRuntime.TargetOutboundAdmission next =
            runtime.admitBoundSessionSend(
                NODE_B, 4, target, outboundPayload("next"));
        AtomicInteger settlements = new AtomicInteger();
        failed.settlement().whenComplete(
            (ignored, failure) -> settlements.incrementAndGet());
        runtime.applyRelocationRouteCommand(route(relocation))
            .toCompletableFuture().join();

        failedTerminal.completeExceptionally(
            new IllegalStateException("physical submit failed"));
        awaitCount(asyncSubmits, 2);

        assertEquals(
            ZLinkSessionActorsRuntime.TargetOutboundSettlement.REJECTED,
            failed.settlement().toCompletableFuture().join());
        assertEquals(1, settlements.get());
        assertFalse(next.settlement().toCompletableFuture().isDone());
        assertEquals(0, syncSubmits.get());

        nextTerminal.complete(null);
        assertEquals(
            ZLinkSessionActorsRuntime.TargetOutboundSettlement.DELIVERED,
            next.settlement().toCompletableFuture().join());
    }

    @Test
    void boundSessionBindWaitsForBackendPhysicalTerminal() throws Exception {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicInteger asyncSubmits = new AtomicInteger();
        ZLinkBackendStreamSocket stream = streamProxy((method, arguments) -> {
            if (method.getName().equals("relayBoundActorAsync")) {
                asyncSubmits.incrementAndGet();
                return physicalTerminal;
            }
            throw new UnsupportedOperationException(method.getName());
        });
        ZLinkBoundSessionRuntime runtime = new ZLinkBoundSessionRuntime(
            stream,
            null,
            SESSION,
            "actor-1",
            null,
            null,
            null,
            ZLinkStreamCodec.JSON,
            ignored -> true,
            ZLinkRelayMetadataPolicy.EMPTY);
        Method relay = ZLinkBoundSessionRuntime.class.getDeclaredMethod(
            "relayBoundSessionBind",
            ZLinkStreamHeader.class);
        relay.setAccessible(true);

        @SuppressWarnings("unchecked")
        CompletionStage<Void> submission = (CompletionStage<Void>) relay.invoke(
            runtime,
            oneWayHeader("bound-session-bind"));

        assertEquals(1, asyncSubmits.get());
        assertFalse(submission.toCompletableFuture().isDone());
        IllegalStateException rejected =
            new IllegalStateException("physical submit rejected");
        physicalTerminal.completeExceptionally(rejected);
        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> submission.toCompletableFuture().join());
        assertSame(rejected, failure.getCause());
    }

    @Test
    void localActorReplyWaitsForBackendPhysicalTerminal() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicReference<ZLinkStreamHeader> submittedHeader =
            new AtomicReference<>();
        AtomicReference<String> submittedPayload = new AtomicReference<>();
        ZLinkBackendStreamSocket stream = streamProxy((method, arguments) -> {
            if (method.getName().equals("replyAsync")) {
                submittedHeader.set((ZLinkStreamHeader) arguments[1]);
                @SuppressWarnings("unchecked")
                List<Message> parts = (List<Message>) arguments[2];
                submittedPayload.set(parts.getFirst().toUtf8String());
                return physicalTerminal;
            }
            throw new UnsupportedOperationException(method.getName());
        });
        ZLinkSessionRelayHeaders relayHeaders = new ZLinkSessionRelayHeaders();
        ZLinkActor managed = () -> null;
        ZLinkBoundActor actor = new ZLinkBoundActor(
            stream,
            SESSION,
            new ZLinkBackendActorRef(
                RoutingId.from("actor-node"), "actor-1", 7),
            "game",
            Optional.of(managed),
            null,
            new RawSerializer(),
            0,
            1,
            ignored -> true,
            (ignoredActor, ignoredSequence, ignoredHeader, ignoredPayload) ->
                CompletableFuture.completedFuture(Optional.of(
                    new ZLinkSessionActorsRuntime.LocalActorReply(
                        Message.from("reply"),
                        ZLinkStreamCodec.PROTOBUF))),
            true,
            ZLinkStreamCodec.JSON,
            relayHeaders,
            null,
            () -> true,
            operation -> operation.apply(1),
            ZLinkRelayMetadataPolicy.EMPTY);
        relayHeaders.enter(new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(41L),
            "Request",
            Map.of()));
        try {
            CompletionStage<Void> submission = actor.relay(
                ZLinkMessage.of("request"));

            assertFalse(submission.toCompletableFuture().isDone());
            assertEquals(ZLinkStreamCodec.PROTOBUF,
                submittedHeader.get().codec());
            assertEquals("reply", submittedPayload.get());
            physicalTerminal.complete(null);
            submission.toCompletableFuture().join();
        } finally {
            relayHeaders.exit();
        }
    }

    @Test
    void boundSessionSendWaitsForBackendPhysicalTerminal() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicReference<ZLinkStreamHeader> submittedHeader =
            new AtomicReference<>();
        AtomicReference<String> submittedPayload = new AtomicReference<>();
        ZLinkBackendStreamSocket stream = streamProxy((method, arguments) -> {
            if (method.getName().equals("sendAsync")) {
                submittedHeader.set((ZLinkStreamHeader) arguments[1]);
                @SuppressWarnings("unchecked")
                List<Message> parts = (List<Message>) arguments[2];
                submittedPayload.set(parts.getFirst().toUtf8String());
                return physicalTerminal;
            }
            throw new UnsupportedOperationException(method.getName());
        });
        ZLinkBoundSessionRuntime runtime = new ZLinkBoundSessionRuntime(
            stream,
            null,
            SESSION,
            "actor-1",
            new RawSerializer(),
            null,
            null,
            ZLinkStreamCodec.JSON,
            ignored -> true,
            ZLinkRelayMetadataPolicy.EMPTY);

        CompletionStage<Void> submission = runtime.send("payload").submit();

        assertFalse(submission.toCompletableFuture().isDone());
        assertEquals(ZLinkStreamMessageKind.SEND,
            submittedHeader.get().kind());
        assertEquals("String", submittedHeader.get().packetName());
        assertEquals("payload", submittedPayload.get());
        physicalTerminal.complete(null);
        submission.toCompletableFuture().join();
    }

    private static ZLinkStreamHeader oneWayHeader(String packetName) {
        return new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.RAW,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            packetName,
            Map.of());
    }

    private static ZLinkSessionActorsRuntime runtime(
        ZLinkBackendStreamSocket stream) {
        return new ZLinkSessionActorsRuntime(
            stream,
            SESSION,
            null,
            new RawSerializer(),
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.RAW);
    }

    private static ZLinkBackendStreamSocket relocationStream(
        AtomicInteger asyncSubmits,
        AtomicInteger syncSubmits,
        ArrayDeque<CompletableFuture<Void>> terminals) {
        return streamProxy((method, arguments) -> switch (method.getName()) {
            case "boundActorBindingGeneration" -> BINDING_GENERATION;
            case "bindActor" -> (ZLinkBackendActorBindOperation) timeout ->
                CompletableFuture.completedFuture(null);
            case "relocateBoundActor" ->
                CompletableFuture.completedFuture(null);
            case "requestBoundActor", "requestExactActor" ->
                CompletableFuture.completedFuture(
                    List.of(Message.from(new byte[0])));
            case "unbindActor" -> (ZLinkBackendActorUnbindOperation) timeout ->
                CompletableFuture.completedFuture(null);
            case "sendBoundSessionPushAsync" -> {
                asyncSubmits.incrementAndGet();
                yield terminals.removeFirst();
            }
            case "sendBoundSessionPush" -> {
                syncSubmits.incrementAndGet();
                throw new IllegalStateException(
                    "Framework must not retry a physical STREAM submit");
            }
            default -> defaultValue(method.getReturnType());
        });
    }

    private static ZLinkServiceM6BWireCodec.RelocationIdentity relocation() {
        return new ZLinkServiceM6BWireCodec.RelocationIdentity(8, 9);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationSeal seal(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
            relocation,
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4"),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(NODE_A, "actor-1", 7),
                3, 9, 4),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION,
                BINDING_GENERATION));
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute route(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            relocation,
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4"),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION,
                BINDING_GENERATION),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            9, 10, NODE_B, 4);
    }

    private static ZLinkServiceM6BWireCodec.BoundSessionSend
        targetBoundSend() {
        return new ZLinkServiceM6BWireCodec.BoundSessionSend(
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(NODE_B, "actor-1", 7),
                4, 10, 4),
            BINDING_GENERATION);
    }

    private static ZLinkServiceM6AWireCodec.ApplicationPayload outboundPayload(
        String value) {
        try (Message payload = Message.from(value)) {
            return ZLinkServiceM6AWireCodec.encodeFrameworkMultipart(
                List.of(payload));
        }
    }

    private static void awaitCount(AtomicInteger value, int expected) {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (value.get() < expected && System.nanoTime() < deadline) {
            Thread.onSpinWait();
        }
        assertEquals(expected, value.get());
    }

    private static ZLinkBackendStreamSocket streamProxy(Invocation invocation) {
        return (ZLinkBackendStreamSocket) Proxy.newProxyInstance(
            ZLinkBackendStreamSocket.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendStreamSocket.class},
            (proxy, method, arguments) -> invocation.invoke(method, arguments));
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) {
            return false;
        }
        if (type == char.class) {
            return '\0';
        }
        return 0;
    }

    @FunctionalInterface
    private interface Invocation {
        Object invoke(Method method, Object[] arguments) throws Throwable;
    }

    private static final class RawSerializer
        implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(
                value.toString().getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(
            ZLinkEncodedPayload payload,
            Class<T> type) {
            throw new UnsupportedOperationException();
        }
    }
}
