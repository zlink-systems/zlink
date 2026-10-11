package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.service.*;

import java.util.List;
import java.util.concurrent.*;

final class ZLinkInstanceActivationShutdownTest {
    @Test
    void closeWaitsForAcceptedFirstMessageStoreCleanup() throws Exception {
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "shutdown")) {
            node.setRoutingId(RoutingId.from("shutdown-node"));
            node.setBind("tcp://127.0.0.1:0");
            node.start();
            var spots = (ZLinkJavaRawSpotNode) node.spotNode();
            var cleanupEntered = new CompletableFuture<Void>();
            var cleanupRelease = new CompletableFuture<Void>();
            var cleanupFinished = new CompletableFuture<Void>();
            var failure = new CompletableFuture<Throwable>();
            var route =
                    new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                            node.routingId(),
                            node.lifecycleGeneration(),
                            "room",
                            1,
                            "owner",
                            1,
                            1,
                            "v1");
            spots.registerInstanceSpotType(
                    "room-type",
                    new ZLinkInternalMeshNode.InstanceSpotActivationHandler() {
                        @Override
                        public CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence> reserve(
                                ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope,
                                ZLinkAuthorityReadResult authority) {
                            return CompletableFuture.completedFuture(route);
                        }

                        @Override
                        public CompletionStage<Void> activate(
                                String stableType,
                                ZLinkServiceM6BWireCodec.InstanceRouteFence authority,
                                ZLinkBackendSpot spot) {
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
                                                CompletableFuture<Void> admitted) {
                                            admitted.complete(null);
                                            received.close();
                                            return CompletableFuture.completedFuture(null);
                                        }
                                    });
                            return CompletableFuture.completedFuture(null);
                        }

                        @Override
                        public CompletionStage<Void> completed(
                                ZLinkServiceM6BWireCodec.InstanceSpotMessage message) {
                            cleanupEntered.complete(null);
                            return cleanupRelease.thenRun(() -> cleanupFinished.complete(null));
                        }
                    });
            var header =
                    new ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                            0,
                            new ZLinkServiceM6BWireCodec.InstanceColdActivation(
                                    node.routingId(),
                                    node.lifecycleGeneration(),
                                    "room",
                                    "shutdown",
                                    "room-type",
                                    "1",
                                    System.currentTimeMillis() + 5000),
                            true,
                            node.lifecycleGeneration(),
                            node.routingId(),
                            null,
                            false,
                            1575,
                            1,
                            null);
            assertTrue(
                    spots.enqueueRemoteInstanceSpot(
                            node.routingId(),
                            header,
                            new byte[0],
                            List.of(Message.from("first")),
                            "application/json",
                            ignored -> {},
                            failure::complete));
            cleanupEntered.get(5, TimeUnit.SECONDS);
            var terminals = spots.instanceActivationTerminals().toCompletableFuture();
            assertFalse(terminals.isDone(), "shutdown can close before Store cleanup");
            cleanupRelease.complete(null);
            terminals.get(5, TimeUnit.SECONDS);
            assertTrue(cleanupFinished.isDone(), "terminal completed before Store cleanup");
            spots.close();
            assertFalse(failure.isDone());
        }
    }
}
