package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

final class ZLinkFrameworkRuntimeManualPeerTest {
    @Test
    void endpointOnlyManualPeerKeepsItsIntentUntilHandshakeAdmissionAndExplicitRemoval()
            throws Exception {
        String meshName = "manual-peer";
        String channelName = "profile";
        RoutingId targetRid = RoutingId.from("manual-peer-a-target");
        RoutingId sourceRid = RoutingId.from("manual-peer-z-source");
        var store = new ZLinkInMemoryProviderLocationStore();

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        var targetMesh =
                targetOptions
                        .addRouteMesh(meshName)
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(targetRid);
        targetMesh.objects().server();
        targetMesh.channelName(channelName).server();

        try (ZLinkFrameworkRuntime target =
                ZLinkFrameworkRuntimeTestAccess.start(
                        targetOptions, new ZLinkJavaBackendAdapterFactory())) {
            await(target::isReady, Duration.ofSeconds(3));
            String targetEndpoint = target.monitoringMeshNode(meshName).status().localEndpoint();

            var sourceOptions = new DefaultZLinkFrameworkOptions();
            sourceOptions.addLocationStore(store);
            var sourceMesh =
                    sourceOptions
                            .addRouteMesh(meshName)
                            .listen("tcp://127.0.0.1:0")
                            .setRoutingId(sourceRid);
            sourceMesh.objects().client();
            sourceMesh.channelName(channelName).client();
            sourceMesh.peerConnections().connect(targetEndpoint);

            try (ZLinkFrameworkRuntime source =
                    ZLinkFrameworkRuntimeTestAccess.start(
                            sourceOptions, new ZLinkJavaBackendAdapterFactory())) {
                await(source::isReady, Duration.ofSeconds(3));
                ZLinkInternalMeshNode sourceNode = source.monitoringMeshNode(meshName);
                long configuredIntentId = sourceNode.connectionIntentIds().getFirst();

                await(
                        () ->
                                sourceNode.peers().stream()
                                        .anyMatch(
                                                peer ->
                                                        peer.routingId().equals(targetRid)
                                                                && peer.state()
                                                                        == MeshPeerState.ADMITTED),
                        Duration.ofSeconds(3));

                assertEquals(List.of(configuredIntentId), sourceNode.connectionIntentIds());
                assertEquals(1L, configuredIntentId);
                assertFalse(sourceNode.isPeerConnectionClosing(configuredIntentId));
                assertEquals(1L, sourceNode.readyChannelMemberCount(channelName));

                sourceNode.removePeerConnection(configuredIntentId);

                await(sourceNode.connectionIntentIds()::isEmpty, Duration.ofSeconds(3));
                assertTrue(sourceNode.connectionIntentIds().isEmpty());
            }
        }
    }

    private static void await(BooleanSupplier condition, Duration timeout) throws Exception {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            TimeUnit.MILLISECONDS.sleep(10);
        }
        assertTrue(condition.getAsBoolean(), "condition was not met within " + timeout);
    }
}
