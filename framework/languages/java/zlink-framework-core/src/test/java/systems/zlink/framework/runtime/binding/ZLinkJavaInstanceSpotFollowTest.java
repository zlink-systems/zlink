package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalAsyncSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.InstanceRouteFence;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.InstanceSpotFollow;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SpotRouteFence;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkJavaInstanceSpotFollowTest {
    @Test
    void readyOneWayFollowsCommittedRouteWithOriginalSender() throws Exception {
        try (var fixture = new Fixture()) {
            fixture.install(fixture.oldFence(), fixture.newFence());
            fixture.send(fixture.oldRoute);
            var expected =
                    new Delivery(
                            fixture.caller.routingId(),
                            fixture.caller.lifecycleGeneration(),
                            "origin",
                            false,
                            0,
                            0,
                            Map.of("trace", "original"),
                            "payload");
            assertEquals(
                    expected,
                    fixture.deliveries.poll(2, TimeUnit.SECONDS),
                    () ->
                            "Ready one-way did not reach the target; previous-owner drops="
                                    + fixture.oldDiagnostics.reportedCount());
            fixture.send(fixture.oldRoute);
            assertEquals(expected, fixture.deliveries.poll(2, TimeUnit.SECONDS));
            assertNull(fixture.deliveries.poll(100, TimeUnit.MILLISECONDS));
            assertEquals(0, fixture.oldDiagnostics.reportedCount());
            assertEquals(0, fixture.targetDiagnostics.reportedCount());
        }
    }

    @Test
    void missingRouteDropsOnce() throws Exception {
        try (var fixture = new Fixture()) {
            fixture.send(fixture.oldRoute);
            fixture.assertDrop(fixture.oldDiagnostics);
        }
    }

    @Test
    void expiredRouteDropsOnce() throws Exception {
        try (var fixture = new Fixture()) {
            fixture.previous
                    .spotNode()
                    .installRelocationSpotForward(
                            fixture.oldFence(), fixture.newFence(), Duration.ofNanos(1));
            fixture.send(fixture.oldRoute);
            fixture.assertDrop(fixture.oldDiagnostics);
        }
    }

    @Test
    void stagedRouteDropsOnce() throws Exception {
        try (var fixture = new Fixture()) {
            fixture.previous
                    .spotNode()
                    .stageRelocationSpotForward(
                            fixture.oldFence(), fixture.newFence(), Duration.ofMinutes(1));
            fixture.send(fixture.oldRoute);
            fixture.assertDrop(fixture.oldDiagnostics);
        }
    }

    @Test
    void exhaustedHopDropsOnceAtAnIntermediateOwner() throws Exception {
        try (var fixture = new Fixture()) {
            fixture.install(fixture.oldFence(), fixture.newFence());
            fixture.sendFollow(fixture.caller, fixture.oldFence(), 8);
            fixture.assertDrop(fixture.oldDiagnostics);
        }
    }

    @Test
    void peerGenerationMismatchDropsOnceAtThePreviousOwner() throws Exception {
        try (var fixture = new Fixture()) {
            var target = fixture.newFence();
            fixture.install(
                    fixture.oldFence(),
                    new SpotRouteFence(
                            target.spotId(),
                            target.spotGeneration(),
                            target.targetNodeRid(),
                            target.targetNodeGeneration() ^ Long.MIN_VALUE,
                            target.authorityOwnerGeneration(),
                            target.ownerLeaseGeneration()));
            fixture.send(fixture.oldRoute);
            fixture.assertDrop(fixture.oldDiagnostics);
        }
    }

    @Test
    void exactTargetFenceRejectsAnEarlierIncarnation() throws Exception {
        try (var fixture = new Fixture()) {
            var target = fixture.newFence();
            fixture.sendFollow(
                    fixture.previous,
                    new SpotRouteFence(
                            target.spotId(),
                            target.spotGeneration() - 1,
                            target.targetNodeRid(),
                            target.targetNodeGeneration(),
                            target.authorityOwnerGeneration(),
                            target.ownerLeaseGeneration()),
                    1);
            fixture.assertDrop(fixture.targetDiagnostics);
        }
    }

    @Test
    void hopZeroIsAProtocolError() throws Exception {
        try (var fixture = new Fixture()) {
            var frames = fixture.followFrames(fixture.oldFence(), 1);
            // The optional source Spot ID is absent. Hop follows its zero-length byte.
            byte[] head = frames.getFirst();
            int hopOffset =
                    5
                            + 2 * Long.BYTES
                            + Long.BYTES
                            + 1
                            + fixture.caller.routingId().toBytes().length
                            + 1;
            assertEquals(1, head[hopOffset]);
            head[hopOffset] = 0;
            var failure =
                    assertThrows(
                            IllegalArgumentException.class,
                            () -> new ZLinkServiceM6BWireCodec().decodeInstanceSpotFollow(frames));
            assertEquals(true, failure.getMessage().contains("messageFollowHopCount"));

            var records = new LinkedBlockingQueue<LogRecord>();
            var logger = Logger.getLogger(ZLinkJavaRawMeshNode.class.getName());
            var handler =
                    new Handler() {
                        @Override
                        public void publish(LogRecord record) {
                            if (record.getMessage().startsWith("ProtocolError")
                                    && fixture.caller
                                            .routingId()
                                            .equals(record.getParameters()[0])) {
                                records.add(record);
                            }
                        }

                        @Override
                        public void flush() {}

                        @Override
                        public void close() {}
                    };
            logger.addHandler(handler);
            try {
                fixture.sendFrames(fixture.caller, fixture.oldFence(), frames);
                assertNotNull(records.poll(2, TimeUnit.SECONDS));
                assertNull(fixture.deliveries.poll(100, TimeUnit.MILLISECONDS));
                assertNull(records.poll());
            } finally {
                logger.removeHandler(handler);
            }
        }
    }

    private static final class Fixture implements AutoCloseable {
        final Context context = Zlink.createContext();
        final ZLinkJavaRawMeshNode caller = node(context, "caller");
        final ZLinkJavaRawMeshNode previous = node(context, "previous");
        final ZLinkJavaRawMeshNode target = node(context, "target");
        final ZLinkDispatchErrorReporter oldDiagnostics = reporter();
        final ZLinkDispatchErrorReporter targetDiagnostics = reporter();
        final LinkedBlockingQueue<Delivery> deliveries = new LinkedBlockingQueue<>();
        final InstanceRouteFence oldRoute;
        final InstanceRouteFence newRoute;

        Fixture() throws Exception {
            previous.start();
            target.start();
            caller.start();
            previous.setDispatchErrorReporter(oldDiagnostics);
            target.setDispatchErrorReporter(targetDiagnostics);
            previous.spotNode()
                    .setSpotAdmissionResolver(
                            (id, generation, intent, present, route) ->
                                    CompletableFuture.completedFuture(
                                            new ZLinkFrameworkException(
                                                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                                                    "previous owner lost authority")));
            oldRoute = route(previous, 7, 11, 3);
            newRoute = route(target, 7, 12, 5);
            var spots = (ZLinkJavaRawSpotNode) target.spotNode();
            spots.registerInstanceSpotAuthority("room", newRoute);
            spots.registerInstanceSpotType(
                    "room",
                    (type, route, spot) -> {
                        spot.onDispatchEvent(
                                new ZLinkInternalAsyncSpotDispatchHandler() {
                                    @Override
                                    public CompletionStage<Void> handleAsync(
                                            ZLinkBackendSpotDispatchInfo info) {
                                        return CompletableFuture.completedFuture(null);
                                    }

                                    @Override
                                    public CompletionStage<Void> handleRoute(
                                            ZLinkBackendReceived received,
                                            CompletableFuture<Void> admission) {
                                        try (received) {
                                            var header = received.activationMessage().orElseThrow();
                                            deliveries.add(
                                                    new Delivery(
                                                            received.routingId().orElseThrow(),
                                                            header.sourceNodeGeneration(),
                                                            received.spotId().orElse(null),
                                                            received.isRequest(),
                                                            header.operationHigh(),
                                                            header.operationLow(),
                                                            ZLinkApplicationMetadata.decode(
                                                                    received.applicationMetadata()),
                                                            received.parts()
                                                                    .getLast()
                                                                    .toUtf8String()));
                                            return CompletableFuture.completedFuture(null);
                                        }
                                    }
                                });
                        return CompletableFuture.completedFuture(null);
                    });
            previous.connectPeer(target.status().localEndpoint(), target.routingId());
            caller.connectPeer(previous.status().localEndpoint(), previous.routingId());
            await(
                    () ->
                            admitted(caller, previous)
                                    && admitted(previous, caller)
                                    && admitted(previous, target)
                                    && admitted(target, previous));
        }

        SpotRouteFence oldFence() {
            return fence(oldRoute);
        }

        SpotRouteFence newFence() {
            return fence(newRoute);
        }

        void install(SpotRouteFence source, SpotRouteFence destination) {
            previous.spotNode()
                    .installRelocationSpotForward(source, destination, Duration.ofMinutes(1));
        }

        void send(InstanceRouteFence route) throws Exception {
            try (var packet = Message.from("Packet");
                    var data = Message.from("payload")) {
                var metadata = ZLinkApplicationMetadata.copyOf(Map.of("trace", "original"));
                caller.sendInstanceSpot(
                                route, "room", "origin", metadata.encode(), List.of(packet, data))
                        .toCompletableFuture()
                        .get(2, TimeUnit.SECONDS);
            }
        }

        List<byte[]> followFrames(SpotRouteFence fence, int hop) {
            var codec = new ZLinkServiceM6BWireCodec();
            try (var packet = Message.from("Packet");
                    var data = Message.from("payload")) {
                return codec.encodeInstanceSpotFollow(
                        new InstanceSpotFollow(
                                0,
                                caller.lifecycleGeneration(),
                                caller.routingId(),
                                null,
                                hop - 1,
                                oldFence()),
                        fence,
                        new byte[0],
                        ZLinkServiceM6AWireCodec.encodeFrameworkMultipartFrame(
                                List.of(packet, data)));
            }
        }

        void sendFollow(ZLinkJavaRawMeshNode sender, SpotRouteFence fence, int hop)
                throws Exception {
            sendFrames(sender, fence, followFrames(fence, hop));
        }

        void sendFrames(ZLinkJavaRawMeshNode sender, SpotRouteFence fence, List<byte[]> frames)
                throws Exception {
            sender.port
                    .send(sender.router, fence.targetNodeRid(), frames)
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
        }

        void assertDrop(ZLinkDispatchErrorReporter reportingOwner) throws Exception {
            await(() -> reportingOwner.reportedCount() > 0);
            assertNull(deliveries.poll(100, TimeUnit.MILLISECONDS));
            assertEquals(1, reportingOwner.reportedCount());
            assertEquals(
                    0,
                    (reportingOwner == oldDiagnostics ? targetDiagnostics : oldDiagnostics)
                            .reportedCount());
        }

        @Override
        public void close() {
            caller.close();
            previous.close();
            target.close();
            context.close();
        }
    }

    private static ZLinkDispatchErrorReporter reporter() {
        return new ZLinkDispatchErrorReporter(
                new ZLinkDispatchOptionsRegistration(),
                ZLinkHandlerActivator.reflection(),
                Runnable::run);
    }

    private static ZLinkJavaRawMeshNode node(Context context, String name) {
        var node = new ZLinkJavaRawMeshNode(context, "mesh");
        node.setRoutingId(RoutingId.from(name));
        node.setBind("inproc://instance-follow-" + name + "-" + System.nanoTime());
        return node;
    }

    private static InstanceRouteFence route(
            ZLinkJavaRawMeshNode node, long object, long authority, long lease) {
        return new InstanceRouteFence(
                node.routingId(),
                node.lifecycleGeneration(),
                "spot",
                object,
                "owner",
                authority,
                lease,
                "store");
    }

    private static SpotRouteFence fence(InstanceRouteFence route) {
        return new SpotRouteFence(
                route.targetSpotId(),
                route.objectGeneration(),
                route.targetNodeRid(),
                route.targetNodeGeneration(),
                route.authorityOwnerGeneration(),
                route.leaseGeneration());
    }

    private static boolean admitted(ZLinkJavaRawMeshNode from, ZLinkJavaRawMeshNode peer) {
        return from.peers().stream()
                .anyMatch(
                        value ->
                                value.routingId().equals(peer.routingId())
                                        && value.state() == MeshPeerState.ADMITTED);
    }

    private static void await(BooleanSupplier condition) throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() >= deadline) {
                throw new AssertionError("service routes did not become ready");
            }
            Thread.sleep(1);
        }
    }

    private record Delivery(
            RoutingId sender,
            long senderGeneration,
            String sourceSpot,
            boolean request,
            long operationHigh,
            long operationLow,
            Map<String, String> metadata,
            String payload) {}
}
