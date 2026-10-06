package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.service.*;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;

import java.util.*;
import java.util.concurrent.*;

final class ZLinkInstanceRecoveryDeadlineTest {
    @Test
    @SuppressWarnings("unchecked")
    void recoveryPassesOriginalAbsoluteDeadlineToActivationHook() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(
                new systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore());
        options.addRouteMesh("recovery")
                .listen("inproc://recovery-" + UUID.randomUUID())
                .objects()
                .server();
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var nodesField = ZLinkSpotRuntime.class.getDeclaredField("routeMeshNodesByName");
            nodesField.setAccessible(true);
            var nodes = (Map<String, ZLinkInternalMeshNode>) nodesField.get(runtime.spotManager());
            var node = nodes.get("recovery");
            var observed = new CompletableFuture<Long>();
            node.registerInstanceSpotType(
                    "recovery",
                    new ZLinkInternalMeshNode.InstanceSpotActivationHandler() {
                        public CompletionStage<Void> activate(
                                String stableType,
                                ZLinkServiceM6BWireCodec.InstanceRouteFence route,
                                ZLinkBackendSpot spot) {
                            observed.completeExceptionally(
                                    new AssertionError(
                                            "recovery discarded its activation deadline"));
                            return CompletableFuture.completedFuture(null);
                        }

                        public CompletionStage<Void> activate(
                                String stableType,
                                ZLinkServiceM6BWireCodec.InstanceRouteFence route,
                                ZLinkBackendSpot spot,
                                long deadlineUnixMs) {
                            observed.complete(deadlineUnixMs);
                            return CompletableFuture.completedFuture(null);
                        }
                    });
            long originalDeadline = System.currentTimeMillis() - 1000;
            var route =
                    new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                            node.routingId(),
                            node.lifecycleGeneration(),
                            "recovered",
                            1,
                            "owner",
                            1,
                            1,
                            "version");
            byte[] payload;
            try (var part = Message.from(new byte[] {1})) {
                payload = ZLinkServiceM6AWireCodec.encodeFrameworkMultipartFrame(List.of(part));
            }
            var envelope =
                    new ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope(
                            "recovered",
                            "recovery",
                            "recovery",
                            node.routingId(),
                            node.lifecycleGeneration(),
                            "1",
                            node.routingId(),
                            node.lifecycleGeneration(),
                            Optional.empty(),
                            false,
                            1,
                            2,
                            null,
                            originalDeadline,
                            new byte[0],
                            payload);
            var recovery = node.recoverInstanceActivation(envelope, route);
            assertEquals(originalDeadline, observed.get(5, TimeUnit.SECONDS));
            ((ZLinkJavaRawSpotNode) node.spotNode()).closeInstanceSpot("recovered", 1);
        }
    }
}
