package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
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
    void immutableSendOptionsShareTheOneWaySubmissionGate() {
        AtomicInteger sends = new AtomicInteger();
        ZLinkInternalSpotNode spotNode = spotNode(sends);
        AtomicReference<Duration> admissionTimeout = new AtomicReference<>();
        AtomicInteger detachedAdmissions = new AtomicInteger();
        ZLinkOneWayCalls.Admission admission = new ZLinkOneWayCalls.Admission() {
            @Override
            public CompletionStage<Void> submit(
                ZLinkBackendObject ignoredBackend,
                ZLinkBackendAdmissionKey ignoredKey,
                Supplier<Boolean> submission,
                Runnable cleanup,
                Duration timeout) {
                admissionTimeout.set(timeout);
                try {
                    if (submission.get()) {
                        return CompletableFuture.completedFuture(null);
                    }
                    return CompletableFuture.failedFuture(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.UNAVAILABLE,
                            "fake transport rejected the send"));
                } finally {
                    cleanup.run();
                }
            }

            @Override
            public CompletionStage<Void> submitDetached(
                ZLinkBackendObject backend,
                ZLinkBackendAdmissionKey key,
                Supplier<Boolean> submission,
                Runnable cleanup,
                Duration timeout) {
                detachedAdmissions.incrementAndGet();
                return submit(backend, key, submission, cleanup, timeout);
            }
        };
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            spotNode,
            Map.of("probe", ProbeFactory.class),
            Map.of(),
            Duration.ofSeconds(1),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection(),
            ZLinkStreamCodec.RAW,
            admission);

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

        base.submit().toCompletableFuture().join();
        CompletionException duplicate = assertThrows(
            CompletionException.class,
            () -> transformed.submit().toCompletableFuture().join());

        assertEquals(ZLinkFrameworkErrorKind.INVALID_OPERATION,
            ((ZLinkFrameworkException) duplicate.getCause()).kind());
        assertEquals(1, sends.get());
        assertEquals(0, detachedAdmissions.get(),
            "a ready bound-session route keeps ordinary send admission");
        assertEquals(Duration.ofSeconds(1), admissionTimeout.get());
    }

    @Test
    void missingBoundSessionRouteFailsWithoutTransportRetry() {
        AtomicInteger sends = new AtomicInteger();
        AtomicInteger attempts = new AtomicInteger();
        ZLinkInternalSpotNode spotNode = spotNode(sends, false);
        ZLinkOneWayCalls.Admission admission =
            (ignoredBackend, ignoredKey, submission, cleanup, timeout) -> {
                attempts.incrementAndGet();
                CompletableFuture<Void> submitted = new CompletableFuture<>();
                try {
                    if (submission.get()) {
                        submitted.complete(null);
                    } else {
                        submitted.completeExceptionally(
                            new AssertionError("missing route was treated as backpressure"));
                    }
                } catch (RuntimeException failure) {
                    submitted.completeExceptionally(failure);
                } finally {
                    cleanup.run();
                }
                return ZLinkOneWayCalls.adaptOneWay(submitted);
            };
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            spotNode,
            Map.of("probe", ProbeFactory.class),
            Map.of(),
            Duration.ofSeconds(1),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection(),
            ZLinkStreamCodec.RAW,
            admission);
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
        assertEquals(1, attempts.get());
        assertEquals(0, sends.get());
    }

    @Test
    void bindingGenerationAloneDoesNotInventARelocationPendingFence() {
        AtomicInteger sends = new AtomicInteger();
        AtomicInteger regularAdmissions = new AtomicInteger();
        AtomicBoolean routeAvailable = new AtomicBoolean();
        AtomicReference<Supplier<Boolean>> pendingAttempt = new AtomicReference<>();
        AtomicReference<Runnable> pendingCleanup = new AtomicReference<>();
        ZLinkInternalSpotNode spotNode = spotNode(sends, routeAvailable);
        ZLinkOneWayCalls.Admission admission = new ZLinkOneWayCalls.Admission() {
            @Override
            public CompletionStage<Void> submit(
                ZLinkBackendObject backend,
                ZLinkBackendAdmissionKey key,
                Supplier<Boolean> submission,
                Runnable cleanup,
                Duration timeoutOverride) {
                regularAdmissions.incrementAndGet();
                if (submission.get()) {
                    cleanup.run();
                }
                return CompletableFuture.completedFuture(null);
            }

            @Override
            public CompletionStage<Void> submitDetached(
                ZLinkBackendObject backend,
                ZLinkBackendAdmissionKey key,
                Supplier<Boolean> submission,
                Runnable cleanup,
                Duration timeoutOverride) {
                if (submission.get()) {
                    cleanup.run();
                } else {
                    pendingAttempt.set(submission);
                    pendingCleanup.set(cleanup);
                }
                return CompletableFuture.completedFuture(null);
            }

            @Override
            public void releaseDetached(
                ZLinkBackendObject backend,
                ZLinkBackendAdmissionKey key) {
                if (pendingAttempt.get().get()) {
                    pendingCleanup.get().run();
                }
            }
        };
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            spotNode,
            Map.of("probe", ProbeFactory.class),
            Map.of(),
            Duration.ofSeconds(1),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection(),
            ZLinkStreamCodec.RAW,
            admission);
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

        assertEquals(1, regularAdmissions.get());
        assertEquals(1, sends.get());
    }

    @Test
    void synchronousJoinWithABoundSessionFailsBeforeCommand42() {
        AtomicInteger sends = new AtomicInteger();
        ZLinkInternalSpotNode spotNode = spotNode(sends);
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            spotNode,
            Map.of("probe", ProbeFactory.class),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer());
        ZLinkActor actor = actors
            .getOrCreateLocalActor("actor-1", ZLinkActor.class)
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
            () -> actors.directJoinSessionRouteCommand(
                    actor,
                    actors.currentRef(actor),
                    RoutingId.from("node-b"),
                    UUID.randomUUID(),
                    false)
                .toCompletableFuture()
                .join());

        assertEquals(
            "StateIncompatible: bound-Session direct Join requires durable "
                + "source-cleanup completion evidence",
            failure.getCause().getMessage());
        assertEquals(0, sends.get(),
            "the unsupported Join must fail before any relocation ingress");
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
            9,
            0);
    }

    private static ZLinkInternalSpotNode spotNode(
        AtomicInteger sends,
        boolean hasBoundSessionRoute) {
        return spotNode(sends, new AtomicBoolean(hasBoundSessionRoute));
    }

    private static ZLinkInternalSpotNode spotNode(
        AtomicInteger sends,
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
                    sends.incrementAndGet();
                    yield true;
                }
                case "boundSessionRoute" -> hasBoundSessionRoute.get()
                    ? Optional.of(new ZLinkInternalSpotNode.BoundSessionRoute(
                        RoutingId.from("session-node"),
                        1,
                        RoutingId.from("session-1"),
                        1,
                        0))
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
