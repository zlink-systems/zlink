package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ZLinkActorRuntimeDirectJoinSessionAbortTest {
    private static final RoutingId SOURCE = RoutingId.from("source");
    private static final RoutingId TARGET = RoutingId.from("target");
    private static final RoutingId SESSION_OWNER =
        RoutingId.from("session-owner");
    private static final RoutingId SESSION = RoutingId.from("session");

    @Test
    void exactSourceAbortIsSingleFlightUntilCommand45AndThenReleasable() {
        var codec = new ZLinkServiceM6BWireCodec();
        AtomicInteger submissions = new AtomicInteger();
        AtomicReference<CompletableFuture<byte[]>> response =
            new AtomicReference<>(new CompletableFuture<>());
        AtomicReference<ZLinkServiceM6BWireCodec.SessionRelocationRoute>
            observed = new AtomicReference<>();
        Nodes nodes = nodes((target, command44) -> {
            submissions.incrementAndGet();
            var abort = codec.decodeSessionRelocationRoute(command44);
            observed.set(abort);
            return response.get();
        });
        var peer = new ZLinkSessionRelocationPeerClient(nodes.mesh(), codec);
        var context = context(codec, peer);
        ZLinkActorRuntime runtime = runtime(nodes.spot());

        CompletableFuture<Void> first = runtime
            .abortDirectJoinSessionRoute(context)
            .toCompletableFuture();
        CompletableFuture<Void> duplicate = runtime
            .abortDirectJoinSessionRoute(context)
            .toCompletableFuture();

        assertSame(first, duplicate);
        assertEquals(1, submissions.get());
        assertEquals(ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            observed.get().senderRole());
        assertEquals(
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
            observed.get().action());
        assertEquals(0, observed.get().lastAcceptedSessionSequence());

        response.get().complete(codec.encodeSessionRelocationRouted(
            ack(observed.get(), 23)));
        first.join();
        duplicate.join();

        response.set(CompletableFuture.completedFuture(
            codec.encodeSessionRelocationRouted(ack(observed.get(), 23))));
        runtime.abortDirectJoinSessionRoute(context).toCompletableFuture().join();
        assertEquals(2, submissions.get(),
            "a terminal ACK removes the bounded single-flight owner");
    }

    @Test
    void runtimeShutdownSettlesAPendingAbortFlight() {
        var codec = new ZLinkServiceM6BWireCodec();
        Nodes nodes = nodes((target, command44) ->
            new CompletableFuture<>());
        var peer = new ZLinkSessionRelocationPeerClient(nodes.mesh(), codec);
        ZLinkActorRuntime runtime = runtime(nodes.spot());
        CompletableFuture<Void> pending = runtime
            .abortDirectJoinSessionRoute(context(codec, peer))
            .toCompletableFuture();

        runtime.closeAsync().toCompletableFuture().join();
        CompletionException failure = assertThrows(
            CompletionException.class,
            pending::join);

        assertEquals(
            "Actor runtime closed with a pending direct-Join Session abort",
            failure.getCause().getMessage());
    }

    @Test
    void runtimeDrainSettlesAPendingAbortFlightBeforeClose() {
        var codec = new ZLinkServiceM6BWireCodec();
        Nodes nodes = nodes((target, command44) ->
            new CompletableFuture<>());
        var peer = new ZLinkSessionRelocationPeerClient(nodes.mesh(), codec);
        ZLinkActorRuntime runtime = runtime(nodes.spot());
        CompletableFuture<Void> pending = runtime
            .abortDirectJoinSessionRoute(context(codec, peer))
            .toCompletableFuture();

        runtime.beginDrain();
        CompletionException failure = assertThrows(
            CompletionException.class,
            pending::join);

        assertEquals(
            "Actor runtime began drain with a pending direct-Join Session abort",
            failure.getCause().getMessage());
        runtime.closeAsync().toCompletableFuture().join();
    }

    private static ZLinkActorRuntime runtime(ZLinkInternalSpotNode node) {
        return new ZLinkActorRuntime(
            node,
            Map.of(),
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer());
    }

    private static ZLinkActorRuntime.DirectJoinSessionRouteCommand context(
        ZLinkServiceM6BWireCodec codec,
        ZLinkSessionRelocationPeerClient peer) {
        var seal = new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(1, 2),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 3, SOURCE, 4, "store-v5"),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef(SOURCE, "actor", 6),
                4,
                10,
                11),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                SESSION_OWNER, 7, "session-owner", 8, SESSION, 9));
        var commit = new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            seal.relocation(),
            seal.coordinator(),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor", 6),
            seal.session(),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            10,
            12,
            TARGET,
            13,
            23);
        return new ZLinkActorRuntime.DirectJoinSessionRouteCommand(
            codec.encodeSessionRelocationRoute(commit),
            seal,
            23,
            peer);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRouted ack(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute abort,
        long highWater) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
            abort.relocation(),
            abort.coordinator(),
            abort.actor(),
            abort.session(),
            abort.action(),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.APPLIED,
            abort.currentAuthorityOwnerGeneration(),
            highWater);
    }

    private static Nodes nodes(Sender sender) {
        ZLinkInternalSpotNode spot = (ZLinkInternalSpotNode)
            Proxy.newProxyInstance(
            ZLinkActorRuntimeDirectJoinSessionAbortTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (ignoredProxy, method, arguments) -> switch (method.getName()) {
                case "routingId" -> SOURCE;
                case "toString" -> "direct-join-abort-spot";
                default -> defaultValue(method.getReturnType());
            });
        ZLinkInternalMeshNode mesh = (ZLinkInternalMeshNode)
            Proxy.newProxyInstance(
            ZLinkActorRuntimeDirectJoinSessionAbortTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (ignoredProxy, method, arguments) -> switch (method.getName()) {
                case "routingId" -> SOURCE;
                case "requestSessionRelocationRoute" -> sender.send(
                    (RoutingId) arguments[0], (byte[]) arguments[1]);
                case "toString" -> "direct-join-abort-node";
                default -> defaultValue(method.getReturnType());
            });
        return new Nodes(spot, mesh);
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) return false;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0F;
        if (type == double.class) return 0D;
        if (type == char.class) return '\0';
        return null;
    }

    @FunctionalInterface
    private interface Sender {
        CompletableFuture<byte[]> send(RoutingId target, byte[] command);
    }

    private record Nodes(
        ZLinkInternalSpotNode spot,
        ZLinkInternalMeshNode mesh) {
    }
}
