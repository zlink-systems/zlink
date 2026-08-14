package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

final class ZLinkSessionRelocationPeerClientTest {
    private static final RoutingId SOURCE = RoutingId.from("source");
    private static final RoutingId TARGET = RoutingId.from("target");
    private static final RoutingId SESSION_OWNER =
        RoutingId.from("session-owner");
    private static final RoutingId SESSION = RoutingId.from("session");

    @Test
    void command44IsSubmittedOnceAsOneWayControl() {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        AtomicInteger sends = new AtomicInteger();
        ZLinkInternalMeshNode node = proxy((method, arguments) -> {
            assertEquals("sendSessionRelocationRoute", method);
            assertEquals(SESSION_OWNER, arguments[0]);
            assertEquals(command,
                codec.decodeSessionRelocationRoute((byte[]) arguments[1]));
            sends.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        });

        new ZLinkSessionRelocationPeerClient(node, codec)
            .sendRoute(command).toCompletableFuture().join();

        assertEquals(1, sends.get());
    }

    @Test
    void command44SubmitFailureIsReturnedWithoutAckRetry() {
        AtomicInteger sends = new AtomicInteger();
        ZLinkInternalMeshNode node = proxy((method, arguments) -> {
            sends.incrementAndGet();
            return CompletableFuture.failedFuture(
                new IllegalStateException("disconnected"));
        });

        assertThrows(CompletionException.class, () ->
            new ZLinkSessionRelocationPeerClient(node)
                .sendRoute(command()).toCompletableFuture().join());
        assertEquals(1, sends.get());
    }

    @Test
    void command43MustEchoCommand42Exactly() {
        var codec = new ZLinkServiceM6BWireCodec();
        var seal = seal();
        ZLinkInternalMeshNode node = proxy((method, arguments) -> {
            assertEquals("requestSessionRelocationSeal", method);
            assertEquals(seal,
                codec.decodeSessionRelocationSeal((byte[]) arguments[1]));
            var echoed = new ZLinkServiceM6BWireCodec
                .SessionRelocationSealed(
                    seal.relocation(), seal.coordinator(), seal.actor(),
                    seal.session());
            return CompletableFuture.completedFuture(
                codec.encodeSessionRelocationSealed(echoed));
        });

        var sealed = new ZLinkSessionRelocationPeerClient(node, codec)
            .sealRouteUntilAck(seal, Duration.ofSeconds(1))
            .toCompletableFuture().join();

        assertEquals(seal.relocation(), sealed.relocation());
        assertEquals(seal.coordinator(), sealed.coordinator());
        assertEquals(seal.actor(), sealed.actor());
        assertEquals(seal.session(), sealed.session());
    }

    private static ZLinkInternalMeshNode proxy(Invocation invocation) {
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkSessionRelocationPeerClientTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("toString")) return "peer";
                return invocation.invoke(method.getName(), arguments);
            });
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationSeal seal() {
        return new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(1, 2),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 3, SOURCE, 4, "store-v5"),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef(SOURCE, "actor", 6),
                4, 10, 11),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                SESSION_OWNER, 7, "session-owner", 8, SESSION, 9));
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute command() {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(1, 2),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 3, SOURCE, 4, "store-v5"),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor", 6),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                SESSION_OWNER, 7, "session-owner", 8, SESSION, 9),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            10, 11, TARGET, 12);
    }

    @FunctionalInterface
    private interface Invocation {
        Object invoke(String method, Object[] arguments);
    }
}
