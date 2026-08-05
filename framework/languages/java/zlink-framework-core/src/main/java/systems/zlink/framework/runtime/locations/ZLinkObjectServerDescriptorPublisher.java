package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkMeshNodeMonitoringProjection;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;

/**
 * Publishes MeshNode discovery descriptors under the host owner lease.
 */
public final class ZLinkObjectServerDescriptorPublisher {
    private final ZLinkLocationRepository store;
    private final ZLinkLocationRuntime runtime;
    private final ZLinkFrameworkRegistration registration;
    private final java.util.Map<String, ZLinkInternalMeshNode> nodes;
    private final AtomicLong revision = new AtomicLong(1);
    private final java.util.concurrent.atomic.AtomicBoolean published =
        new java.util.concurrent.atomic.AtomicBoolean();

    public ZLinkObjectServerDescriptorPublisher(
        ZLinkLocationRepository store,
        ZLinkLocationRuntime runtime,
        ZLinkFrameworkRegistration registration,
        java.util.Map<String, ZLinkInternalMeshNode> nodes) {
        this.store = store;
        this.runtime = runtime;
        this.registration = registration;
        this.nodes = java.util.Map.copyOf(nodes);
    }

    public CompletionStage<Void> publish(ZLinkFrameworkRuntimeState state) {
        ZLinkLocationOwnerToken owner = runtime.currentOwnerToken();
        ZLinkLocationWriteIntent intent = published.get()
            ? ZLinkLocationWriteIntent.RENEW
            : ZLinkLocationWriteIntent.NEW_CLAIM;
        List<CompletionStage<?>> writes = new ArrayList<>();
        for (MeshNodeRegistration configured : registration.meshNodes()) {
            ZLinkInternalMeshNode node = nodes.get(configured.meshName());
            if (node == null) {
                continue;
            }
            writes.add(store.updateMeshNode(
                    descriptor(configured, node, owner, state),
                    intent)
                .thenAccept(result -> {
                    if (result.status() != ZLinkLocationWriteStatus.STORED) {
                        throw new systems.zlink.framework.errors
                            .ZLinkConfigurationException(
                                "MeshNode descriptor publication failed"
                                    + " [mesh=" + configured.meshName()
                                    + ", status=" + result.status() + "]");
                    }
                }));
        }
        return all(writes).thenRun(() -> published.set(true));
    }

    public CompletionStage<Void> remove() {
        ZLinkLocationOwnerToken owner = runtime.currentOwnerToken();
        List<CompletionStage<?>> removals = new ArrayList<>();
        for (MeshNodeRegistration configured : registration.meshNodes()) {
            ZLinkInternalMeshNode node = nodes.get(configured.meshName());
            if (node != null) {
                removals.add(store.removeMeshNode(
                    new ZLinkMeshNodeDescriptorKey(
                        configured.meshName(),
                        node.status().routingId()),
                    owner));
            }
        }
        return all(removals);
    }

    private ZLinkMeshNodeDescriptor descriptor(
        MeshNodeRegistration configured,
        ZLinkInternalMeshNode node,
        ZLinkLocationOwnerToken owner,
        ZLinkFrameworkRuntimeState state) {
        long descriptorRevision = revision.getAndIncrement();
        ZLinkMeshNodeMonitoringProjection placement =
            ZLinkMeshNodeMonitoringProjection.fromRegistration(
                configured,
                descriptorRevision,
                state == ZLinkFrameworkRuntimeState.SERVING
                    ? node.placementWeight()
                    : 0);
        return new ZLinkMeshNodeDescriptor(
            configured.meshName(),
            node.status().routingId(),
            node.status().lifecycleGeneration(),
            descriptorRevision,
            configured.advertisedEndpoint(node.status().localEndpoint()),
            node.channelWeights(),
            registration.applicationVersion(),
            placement.objectCapabilities(),
            configured.objectServer()
                ? ZLinkMeshNodeObjectRole.SERVER
                : configured.objectRoleEnabled()
                    ? ZLinkMeshNodeObjectRole.CLIENT
                    : ZLinkMeshNodeObjectRole.NONE,
            configured.objectServer()
                ? Optional.of(configured.entrySpotId())
                : Optional.empty(),
            placement.placementWeight(),
            placement.objectCapacity(),
            new ZLinkActivationConcurrency(
                placement.activationConcurrency().active(),
                placement.activationConcurrency().limit()),
            registration.maintenanceWave(),
            state,
            node.status().routingId().toString(),
            owner.ownerId(),
            owner.leaseGeneration(),
            Instant.now());
    }

    private static CompletionStage<Void> all(
        List<CompletionStage<?>> stages) {
        return CompletableFuture.allOf(stages.stream()
            .map(CompletionStage::toCompletableFuture)
            .toArray(CompletableFuture[]::new));
    }
}
