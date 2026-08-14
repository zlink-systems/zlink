package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.streams.ZLinkStreamCodec;

final class ZLinkBoundSessionSendCallContractTest {
    @Test
    void localNativeBoundSessionUsesOneBindingAdmissionTerminal() {
        AtomicInteger synchronousSends = new AtomicInteger();
        AtomicInteger asynchronousSends = new AtomicInteger();
        CompletableFuture<Void> bindingTerminal = new CompletableFuture<>();
        ZLinkInternalSpotNode spotNode = spotNode(
            synchronousSends,
            asynchronousSends,
            bindingTerminal,
            new AtomicBoolean(true));
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            spotNode,
            Map.of("probe", ProbeFactory.class),
            Map.of(),
            Duration.ofSeconds(1),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection(),
            ZLinkStreamCodec.RAW,
            unusedAdmission());

        ZLinkActor actor = actors.getOrCreateLocalActor("actor-1", ZLinkActor.class)
            .toCompletableFuture()
            .join()
            .orElseThrow();
        actors.bindNativeSession(
            actor,
            spotNode,
            new ZLinkBackendActorRef(
                RoutingId.from("node-a"),
                "actor-1",
                7));

        ZLinkBoundSession boundSession = actor.context().boundSession();
        ZLinkBoundSessionSendCall base = boundSession.send("payload");
        ZLinkBoundSessionSendCall transformed = base.metadata("trace", "one");

        CompletionStage<Void> submitted = base.submit();
        long evidenceDeadline =
            System.nanoTime() + Duration.ofSeconds(1).toNanos();
        while (asynchronousSends.get() == 0
            && System.nanoTime() < evidenceDeadline) {
            Thread.onSpinWait();
        }
        assertAll(
            () -> assertEquals(1, asynchronousSends.get()),
            () -> assertEquals(0, synchronousSends.get()),
            () -> assertFalse(submitted.toCompletableFuture().isDone()));

        bindingTerminal.complete(null);
        submitted.toCompletableFuture().join();
        CompletionException duplicate = assertThrows(
            CompletionException.class,
            () -> transformed.submit().toCompletableFuture().join());

        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION,
            ((ZLinkFrameworkException) duplicate.getCause()).kind());
        assertEquals(1, asynchronousSends.get());
        assertEquals(0, synchronousSends.get());
    }

    @Test
    void missingBoundSessionRouteFailsWithoutTransportRetry() {
        AtomicInteger sends = new AtomicInteger();
        ZLinkInternalSpotNode spotNode = spotNode(sends, false);
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            spotNode,
            Map.of("probe", ProbeFactory.class),
            Map.of(),
            Duration.ofSeconds(1),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection(),
            ZLinkStreamCodec.RAW,
            unusedAdmission());
        ZLinkActor actor = actors.getOrCreateLocalActor("actor-1", ZLinkActor.class)
            .toCompletableFuture()
            .join()
            .orElseThrow();
        actors.bindNativeSession(
            actor,
            spotNode,
            new ZLinkBackendActorRef(
                RoutingId.from("node-a"),
                "actor-1",
                7));

        CompletionException failure = assertThrows(
            CompletionException.class,
            () -> actor.context().boundSession().send("payload")
                .submit().toCompletableFuture().join());

        assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND,
            ((ZLinkFrameworkException) failure.getCause()).kind());
        assertEquals(0, sends.get());
    }

    @Test
    void bindingGenerationAloneDoesNotInventARelocationPendingFence() {
        AtomicInteger sends = new AtomicInteger();
        AtomicBoolean routeAvailable = new AtomicBoolean();
        ZLinkInternalSpotNode spotNode = spotNode(sends, routeAvailable);
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            spotNode,
            Map.of("probe", ProbeFactory.class),
            Map.of(),
            Duration.ofSeconds(1),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection(),
            ZLinkStreamCodec.RAW,
            unusedAdmission());
        ZLinkActor actor = actors.getOrCreateLocalActor("actor-1", ZLinkActor.class)
            .toCompletableFuture()
            .join()
            .orElseThrow();
        actors.bindNativeSession(
            actor,
            spotNode,
            new ZLinkBackendActorRef(
                RoutingId.from("node-a"),
                "actor-1",
                7),
            RoutingId.from("session-node"),
            RoutingId.from("session-1"),
            11,
            0);

        routeAvailable.set(true);
        actor.context().boundSession().send("payload")
            .submit().toCompletableFuture().join();

        assertEquals(1, sends.get());
    }

    private static java.util.function.BiFunction<
        ZLinkBackendObject,
        ZLinkBackendAdmissionKey,
        java.util.function.BiFunction<
            Supplier<Boolean>,
            Runnable,
            CompletionStage<Void>>> unusedAdmission() {
        return (backend, key) -> (submission, cleanup) ->
            CompletableFuture.failedFuture(new AssertionError(
                "Framework admission must not be invoked"));
    }

    private static ZLinkInternalSpotNode spotNode(AtomicInteger sends) {
        return spotNode(sends, true);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute
        relocationCommand() {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(1, 2),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "source-owner",
                3,
                RoutingId.from("source-node"),
                4,
                "store-v5"),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                RoutingId.from("session-node"),
                5,
                "session-owner",
                6,
                RoutingId.from("session-1"),
                11),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            7,
            8,
            RoutingId.from("node-a"),
            9);
    }

    private static ZLinkInternalSpotNode spotNode(
        AtomicInteger sends,
        boolean hasBoundSessionRoute) {
        return spotNode(sends, new AtomicBoolean(hasBoundSessionRoute));
    }

    private static ZLinkInternalSpotNode spotNode(
        AtomicInteger sends,
        AtomicBoolean hasBoundSessionRoute) {
        return spotNode(
            sends,
            sends,
            CompletableFuture.completedFuture(null),
            hasBoundSessionRoute);
    }

    private static ZLinkInternalSpotNode spotNode(
        AtomicInteger synchronousSends,
        AtomicInteger asynchronousSends,
        CompletionStage<Void> bindingTerminal,
        AtomicBoolean hasBoundSessionRoute) {
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "routingId" -> RoutingId.from("node-a");
                case "createActor" -> {
                    if (arguments[1] instanceof Message request) {
                        request.close();
                    }
                    yield new ZLinkBackendActorRef(
                        RoutingId.from("node-a"),
                        (String) arguments[0],
                        7);
                }
                case "sendActorBoundSession" -> {
                    synchronousSends.incrementAndGet();
                    yield true;
                }
                case "hasRemoteActorBoundSessionRoute" -> false;
                case "hasLocalActorBoundSessionRoute" ->
                    hasBoundSessionRoute.get();
                case "sendLocalActorBoundSession" -> {
                    synchronousSends.incrementAndGet();
                    yield true;
                }
                case "sendLocalActorBoundSessionAsync" -> {
                    asynchronousSends.incrementAndGet();
                    yield bindingTerminal;
                }
                case "sendRemoteActorBoundSession" -> {
                    asynchronousSends.incrementAndGet();
                    yield CompletableFuture.completedFuture(null);
                }
                case "boundSessionRoute" -> hasBoundSessionRoute.get()
                    ? Optional.of(new ZLinkInternalSpotNode.BoundSessionRoute(
                        RoutingId.from("session-node"),
                        1,
                        RoutingId.from("session-1"),
                        1))
                    : Optional.empty();
                case "destroyActor" -> CompletableFuture.completedFuture(null);
                case "close" -> null;
                default -> defaultValue(method.getReturnType());
            });
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) {
            return false;
        }
        if (type == byte.class) {
            return (byte) 0;
        }
        if (type == short.class) {
            return (short) 0;
        }
        if (type == int.class) {
            return 0;
        }
        if (type == long.class) {
            return 0L;
        }
        if (type == float.class) {
            return 0F;
        }
        if (type == double.class) {
            return 0D;
        }
        if (type == char.class) {
            return '\0';
        }
        return null;
    }

    public static final class ProbeFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new ProbeActor(context));
        }
    }

    private record ProbeActor(ZLinkActorContext context) implements ZLinkActor {
    }
}
