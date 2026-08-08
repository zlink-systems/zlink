package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiFunction;
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
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.streams.ZLinkStreamCodec;

final class ZLinkBoundSessionSendCallContractTest {
    @Test
    void immutableSendOptionsShareTheOneWaySubmissionGate() {
        AtomicInteger sends = new AtomicInteger();
        ZLinkInternalSpotNode spotNode = spotNode(sends);
        BiFunction<ZLinkBackendObject, ZLinkBackendAdmissionKey,
            BiFunction<Supplier<Boolean>, Runnable, CompletionStage<Void>>> admission =
            (ignoredBackend, ignoredKey) -> (submission, cleanup) -> {
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
    }

    private static ZLinkInternalSpotNode spotNode(AtomicInteger sends) {
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
