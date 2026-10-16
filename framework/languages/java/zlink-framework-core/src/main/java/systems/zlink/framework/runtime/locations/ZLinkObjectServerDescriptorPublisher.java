package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkMeshNodeMonitoringProjection;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;

import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** Publishes MeshNode discovery descriptors under the host owner lease. */
public final class ZLinkObjectServerDescriptorPublisher {
    private final ZLinkLocationRepository store;
    private final ZLinkLocationRuntime runtime;
    private final ZLinkFrameworkRegistration registration;
    private final Map<String, ZLinkInternalMeshNode> nodes;
    private final AtomicLong revision = new AtomicLong(1);
    private final AtomicReference<Claim> claim = new AtomicReference<>();

    public ZLinkObjectServerDescriptorPublisher(
            ZLinkLocationRepository store,
            ZLinkLocationRuntime runtime,
            ZLinkFrameworkRegistration registration,
            Map<String, ZLinkInternalMeshNode> nodes) {
        this.store = store;
        this.runtime = runtime;
        this.registration = registration;
        this.nodes = Map.copyOf(nodes);
    }

    public CompletionStage<Void> publish(ZLinkFrameworkRuntimeState state) {
        ZLinkLocationOwnerToken owner;
        try {
            owner = runtime.currentOwnerToken();
        } catch (IllegalStateException ownerAdmissionClosed) {
            // Location runtime §5: an expired owner does not publish a descriptor.
            return CompletableFuture.failedFuture(
                    new UnconfirmedPublicationException(ownerAdmissionClosed));
        }
        long leaseGeneration = owner.leaseGeneration();
        Claim current = claim.get();
        boolean sameLease = current != null && current.leaseGeneration() == leaseGeneration;
        ZLinkLocationWriteIntent intent =
                sameLease && current.confirmed()
                        ? ZLinkLocationWriteIntent.RENEW
                        : ZLinkLocationWriteIntent.NEW_CLAIM;
        // Location runtime §10: a claim whose result is unknown is sent again as the same claim.
        List<ZLinkMeshNodeDescriptor> descriptors =
                sameLease && !current.confirmed()
                        ? current.descriptors()
                        : descriptors(owner, state);
        Claim attempt =
                intent == ZLinkLocationWriteIntent.NEW_CLAIM
                        ? new Claim(leaseGeneration, descriptors, false)
                        : current;
        claim.compareAndSet(current, attempt);
        List<CompletionStage<?>> writes = new ArrayList<>();
        for (ZLinkMeshNodeDescriptor descriptor : descriptors) {
            writes.add(
                    CompletableFuture.completedFuture(null)
                            .thenCompose(
                                    ignored ->
                                            runtime.writeDescriptor(
                                                    () -> store.updateMeshNode(descriptor, intent)))
                            .thenAccept(
                                    result -> {
                                        if (result.status() != ZLinkLocationWriteStatus.STORED) {
                                            throw new systems.zlink.framework.errors
                                                    .ZLinkConfigurationException(
                                                    "MeshNode descriptor publication failed"
                                                            + " [mesh="
                                                            + descriptor.meshName()
                                                            + ", status="
                                                            + result.status()
                                                            + "]");
                                        }
                                    }));
        }
        return requirePublicationResults(writes)
                .thenRun(
                        () ->
                                claim.compareAndSet(
                                        attempt, new Claim(leaseGeneration, List.of(), true)));
    }

    public CompletionStage<Void> recoverAfterOwnerLease(
            ZLinkFrameworkRuntimeState state, ZLinkLocationOwnerToken previousOwner) {
        if (previousOwner == null) {
            return publish(state);
        }
        List<CompletionStage<?>> removals = new ArrayList<>();
        for (MeshNodeRegistration configured : registration.meshNodes()) {
            ZLinkInternalMeshNode node = nodes.get(configured.meshName());
            if (node != null) {
                removals.add(
                        CompletableFuture.completedFuture(null)
                                .thenCompose(
                                        ignored ->
                                                store.removeMeshNode(
                                                        new ZLinkMeshNodeDescriptorKey(
                                                                configured.meshName(),
                                                                node.status().routingId()),
                                                        previousOwner)));
            }
        }
        return requirePublicationResults(removals).thenCompose(ignored -> publish(state));
    }

    public CompletionStage<Void> remove() {
        var admitted = runtime.admittedOwnerToken();
        if (admitted.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkLocationOwnerToken owner = admitted.get();
        List<CompletionStage<?>> removals = new ArrayList<>();
        for (MeshNodeRegistration configured : registration.meshNodes()) {
            ZLinkInternalMeshNode node = nodes.get(configured.meshName());
            if (node != null) {
                removals.add(
                        store.removeMeshNode(
                                new ZLinkMeshNodeDescriptorKey(
                                        configured.meshName(), node.status().routingId()),
                                owner));
            }
        }
        return all(removals);
    }

    private List<ZLinkMeshNodeDescriptor> descriptors(
            ZLinkLocationOwnerToken owner, ZLinkFrameworkRuntimeState state) {
        List<ZLinkMeshNodeDescriptor> descriptors = new ArrayList<>();
        for (MeshNodeRegistration configured : registration.meshNodes()) {
            ZLinkInternalMeshNode node = nodes.get(configured.meshName());
            if (node != null) {
                descriptors.add(descriptor(configured, node, owner, state));
            }
        }
        return List.copyOf(descriptors);
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
                        state == ZLinkFrameworkRuntimeState.SERVING ? node.placementWeight() : 0);
        return new ZLinkMeshNodeDescriptor(
                configured.meshName(),
                node.status().routingId(),
                node.status().lifecycleGeneration(),
                descriptorRevision,
                node.advertisedEndpoint(),
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
                //  The published security identity must be the value peers see
                //  in the admission descriptor (the plaintext-transport
                //  placeholder), not this node's routing id: auto-connect turns
                //  this row back into the expected admission identity.
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                owner.ownerId(),
                owner.leaseGeneration(),
                Instant.now());
    }

    /**
     * Classifies the completed batch once: a definite failure takes priority over an unknown
     * result.
     */
    private static CompletionStage<Void> requirePublicationResults(
            List<CompletionStage<?>> operations) {
        List<CompletableFuture<Throwable>> results =
                operations.stream()
                        .map(
                                operation ->
                                        operation
                                                .handle(
                                                        (ignored, failure) -> {
                                                            while (failure
                                                                            instanceof
                                                                            CompletionException
                                                                    && failure.getCause() != null) {
                                                                failure = failure.getCause();
                                                            }
                                                            return failure;
                                                        })
                                                .toCompletableFuture())
                        .toList();
        return all(results)
                .thenCompose(
                        ignored -> {
                            Throwable unavailable = null;
                            for (CompletableFuture<Throwable> result : results) {
                                Throwable failure = result.getNow(null);
                                if (failure == null) {
                                    continue;
                                }
                                if (failure instanceof UnconfirmedPublicationException
                                        || failure instanceof TimeoutException
                                        || failure
                                                        instanceof
                                                        ZLinkFrameworkException frameworkFailure
                                                && frameworkFailure.kind()
                                                        == ZLinkFrameworkErrorKind.UNAVAILABLE) {
                                    unavailable = failure;
                                } else {
                                    return CompletableFuture.failedFuture(failure);
                                }
                            }
                            return unavailable == null
                                    ? CompletableFuture.completedFuture(null)
                                    : CompletableFuture.failedFuture(
                                            unavailable instanceof UnconfirmedPublicationException
                                                    ? unavailable
                                                    : new UnconfirmedPublicationException(
                                                            unavailable));
                        });
    }

    private static CompletionStage<Void> all(List<? extends CompletionStage<?>> stages) {
        return CompletableFuture.allOf(
                stages.stream()
                        .map(CompletionStage::toCompletableFuture)
                        .toArray(CompletableFuture[]::new));
    }

    /**
     * Records the lease generation and pending NewClaim descriptors until the Store confirms
     * publication. Renew starts only after the claim is confirmed.
     */
    private record Claim(
            long leaseGeneration, List<ZLinkMeshNodeDescriptor> descriptors, boolean confirmed) {}

    /**
     * Location runtime §5 and §10: the publication result is unknown, so the host stays Preparing
     * and publishes again at the next owner lease renewal. A definite Store result is reported as a
     * {@link systems.zlink.framework.errors.ZLinkConfigurationException} instead.
     */
    public static final class UnconfirmedPublicationException extends IllegalStateException {
        UnconfirmedPublicationException(Throwable cause) {
            super("MeshNode descriptor publication is not confirmed: " + cause.getMessage(), cause);
        }
    }
}
