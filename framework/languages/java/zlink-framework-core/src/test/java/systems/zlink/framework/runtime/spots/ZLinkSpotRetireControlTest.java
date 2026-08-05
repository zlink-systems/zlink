package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

final class ZLinkSpotRetireControlTest {
    @Test
    void exactDuplicateStageAndPublishJoinOneTargetOperation()
        throws Exception {
        RoutingId source = RoutingId.from("retire-source");
        RoutingId target = RoutingId.from("retire-target");
        AtomicReference<ZLinkInternalMeshNode.RelocationControlHandler>
            handler = new AtomicReference<>();
        ZLinkInternalMeshNode node = loopbackNode(source, handler);
        AtomicInteger stages = new AtomicInteger();
        AtomicReference<ZLinkSpotRetireControl.StageRequest> decoded =
            new AtomicReference<>();
        AtomicInteger publishes = new AtomicInteger();
        AtomicInteger finalizes = new AtomicInteger();
        AtomicInteger aborts = new AtomicInteger();
        ZLinkSpotRetireControl.install(
            node,
            new ZLinkSpotRetireControl.TargetEndpoint() {
                @Override public java.util.concurrent.CompletionStage<Void>
                    stage(ZLinkSpotRetireControl.StageRequest request) {
                    stages.incrementAndGet();
                    decoded.set(request);
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    publish(ZLinkSpotRetireControl.StageRequest request) {
                    publishes.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    abort(ZLinkSpotRetireControl.StageRequest request) {
                    aborts.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    finalizeAfterCompletion(
                        ZLinkSpotRetireControl.StageRequest request) {
                    finalizes.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                }
            });
        var client = ZLinkSpotRetireControl.client(node);
        var request = request(source, target);

        client.stage(target, request, Duration.ofSeconds(1))
            .toCompletableFuture().get(1, TimeUnit.SECONDS);
        client.stage(target, request, Duration.ofSeconds(1))
            .toCompletableFuture().get(1, TimeUnit.SECONDS);
        client.publish(target, request.fence(), Duration.ofSeconds(1))
            .toCompletableFuture().get(1, TimeUnit.SECONDS);
        client.finalizeAfterCompletion(
                target, request.fence(), Duration.ofSeconds(1))
            .toCompletableFuture().get(1, TimeUnit.SECONDS);
        client.finalizeAfterCompletion(
                target, request.fence(), Duration.ofSeconds(1))
            .toCompletableFuture().get(1, TimeUnit.SECONDS);
        client.publish(target, request.fence(), Duration.ofSeconds(1))
            .toCompletableFuture().get(1, TimeUnit.SECONDS);

        assertEquals(1, stages.get());
        assertEquals(request, decoded.get());
        assertEquals(1, publishes.get());
        assertEquals(1, finalizes.get());
        assertEquals(0, aborts.get());
        assertThrows(
            CompletionException.class,
            () -> client.abort(
                    target,
                    request.fence(),
                    Duration.ofSeconds(1))
                .toCompletableFuture()
                .join());
    }

    @Test
    void transportSourceFenceRejectsSpoofedStageBeforeTargetMutation() {
        RoutingId transportSource = RoutingId.from("actual-source");
        RoutingId encodedSource = RoutingId.from("spoofed-source");
        RoutingId target = RoutingId.from("retire-target");
        AtomicReference<ZLinkInternalMeshNode.RelocationControlHandler>
            handler = new AtomicReference<>();
        ZLinkInternalMeshNode node = loopbackNode(transportSource, handler);
        AtomicInteger stages = new AtomicInteger();
        ZLinkSpotRetireControl.install(
            node,
            new ZLinkSpotRetireControl.TargetEndpoint() {
                @Override public java.util.concurrent.CompletionStage<Void>
                    stage(ZLinkSpotRetireControl.StageRequest request) {
                    stages.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    publish(ZLinkSpotRetireControl.StageRequest request) {
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    abort(ZLinkSpotRetireControl.StageRequest request) {
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    finalizeAfterCompletion(
                        ZLinkSpotRetireControl.StageRequest request) {
                    return CompletableFuture.completedFuture(null);
                }
            });

        assertThrows(
            CompletionException.class,
            () -> ZLinkSpotRetireControl.client(node)
                .stage(
                    target,
                    request(encodedSource, target),
                    Duration.ofSeconds(1))
                .toCompletableFuture()
                .join());
        assertEquals(0, stages.get());
    }

    @Test
    void duplicateAbortIsIdempotentButCannotReopenTheStage() {
        RoutingId source = RoutingId.from("abort-source");
        RoutingId target = RoutingId.from("abort-target");
        AtomicReference<ZLinkInternalMeshNode.RelocationControlHandler>
            handler = new AtomicReference<>();
        ZLinkInternalMeshNode node = loopbackNode(source, handler);
        AtomicInteger aborts = new AtomicInteger();
        ZLinkSpotRetireControl.install(
            node,
            new ZLinkSpotRetireControl.TargetEndpoint() {
                @Override public java.util.concurrent.CompletionStage<Void>
                    stage(ZLinkSpotRetireControl.StageRequest request) {
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    publish(ZLinkSpotRetireControl.StageRequest request) {
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    abort(ZLinkSpotRetireControl.StageRequest request) {
                    aborts.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    finalizeAfterCompletion(
                        ZLinkSpotRetireControl.StageRequest request) {
                    return CompletableFuture.completedFuture(null);
                }
            });
        var request = request(source, target);
        var client = ZLinkSpotRetireControl.client(node);
        client.stage(target, request, Duration.ofSeconds(1))
            .toCompletableFuture().join();
        client.abort(target, request.fence(), Duration.ofSeconds(1))
            .toCompletableFuture().join();
        client.abort(target, request.fence(), Duration.ofSeconds(1))
            .toCompletableFuture().join();

        assertEquals(1, aborts.get());
        assertThrows(
            CompletionException.class,
            () -> client.stage(target, request, Duration.ofSeconds(1))
                .toCompletableFuture().join());
    }

    @Test
    void replyRelayRoundTripsClosedAckAndExactTransportSource() {
        RoutingId source = RoutingId.from("relay-source");
        RoutingId target = RoutingId.from("relay-target");
        AtomicReference<ZLinkInternalMeshNode.RelocationControlHandler>
            handler = new AtomicReference<>();
        ZLinkInternalMeshNode node = loopbackNode(target, handler);
        AtomicReference<RoutingId> transportSource = new AtomicReference<>();
        AtomicReference<ZLinkSpotRelocationReplyRoutes.Relay> decoded =
            new AtomicReference<>();
        ZLinkSpotRetireControl.install(
            node,
            new ZLinkSpotRetireControl.TargetEndpoint() {
                @Override public java.util.concurrent.CompletionStage<Void>
                    stage(ZLinkSpotRetireControl.StageRequest request) {
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    publish(ZLinkSpotRetireControl.StageRequest request) {
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    abort(ZLinkSpotRetireControl.StageRequest request) {
                    return CompletableFuture.completedFuture(null);
                }

                @Override public java.util.concurrent.CompletionStage<Void>
                    finalizeAfterCompletion(
                        ZLinkSpotRetireControl.StageRequest request) {
                    return CompletableFuture.completedFuture(null);
                }

                @Override
                public java.util.concurrent.CompletionStage<
                    ZLinkSpotRelocationReplyRoutes.Ack> relayReply(
                        RoutingId actualSource,
                        ZLinkSpotRelocationReplyRoutes.Relay relay) {
                    transportSource.set(actualSource);
                    decoded.set(relay);
                    return CompletableFuture.completedFuture(
                        ZLinkSpotRelocationReplyRoutes.Ack.ALREADY_TERMINAL);
                }
            });
        var fence = new ZLinkSpotRetireControl.Fence(UUID.randomUUID(), 3);
        var relay = new ZLinkSpotRelocationReplyRoutes.Relay(
            new ZLinkSpotRelocationReplyRoutes.OperationId(11, 12),
            13,
            "spot-1",
            14,
            "source-owner",
            15,
            source,
            16,
            17,
            18,
            1,
            List.of(new byte[] {19, 20}));

        var ack = ZLinkSpotRetireControl.client(node)
            .relayReply(source, fence, relay, Duration.ofSeconds(1))
            .toCompletableFuture().join();

        assertEquals(ZLinkSpotRelocationReplyRoutes.Ack.ALREADY_TERMINAL, ack);
        assertEquals(target, transportSource.get());
        assertEquals(relay.operation(), decoded.get().operation());
        assertEquals(relay.replyRouteId(), decoded.get().replyRouteId());
        assertEquals(relay.spotId(), decoded.get().spotId());
        assertEquals(relay.sourceOwnerId(), decoded.get().sourceOwnerId());
        assertEquals(relay.targetNodeGeneration(),
            decoded.get().targetNodeGeneration());
        assertArrayEquals(relay.parts().get(0), decoded.get().parts().get(0));
    }

    private static ZLinkSpotRetireControl.StageRequest request(
        RoutingId source,
        RoutingId target) {
        return new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(UUID.randomUUID(), 1),
            source,
            11,
            "source-owner",
            12,
            target,
            21,
            "target-owner",
            22,
            "mesh",
            "spot-1",
            "room",
            false,
            false,
            "sha256:root",
            0x01020304L,
            List.of(
                new ZLinkSpotRetireControl.ParticipantFence(
                    "zla1:a:actor-1",
                    1,
                    "actor-1",
                    "player",
                    false,
                    7,
                    9),
                new ZLinkSpotRetireControl.ParticipantFence(
                    "zla1:s:spot-1",
                    2,
                    "spot-1",
                    "room",
                    false,
                    3,
                    5)),
            List.of(new ZLinkSpotRetireControl.SessionRouteFence(
                "actor-1",
                7,
                9,
                "store-v4",
                RoutingId.from("session-owner"),
                13,
                "session-owner-id",
                14,
                RoutingId.from("session-1"),
                15,
                16)));
    }

    private static ZLinkInternalMeshNode loopbackNode(
        RoutingId source,
        AtomicReference<ZLinkInternalMeshNode.RelocationControlHandler>
            handler) {
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkInternalMeshNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("setRelocationControlHandler")) {
                    handler.set(
                        (ZLinkInternalMeshNode.RelocationControlHandler)
                            arguments[0]);
                    return null;
                }
                if (method.getName().equals("requestRelocationControl")) {
                    return handler.get().handle(
                        source,
                        ((byte[]) arguments[1]).clone());
                }
                if (method.getDeclaringClass() == Object.class) {
                    return method.invoke(proxy, arguments);
                }
                throw new UnsupportedOperationException(method.getName());
            });
    }
}
