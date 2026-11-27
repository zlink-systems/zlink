package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.LongSupplier;

/**
 * Host-wide settings the queue Instance Spot reads when it runs on this process (§3.5.3: the Spot
 * uses the settings of the node that runs its incarnation). Registered as a runtime service so the
 * Spot constructor receives it.
 */
public final class ZLinkAdmissionQueueHost {
    /** Whether a node owner lease is still current; false means it ended (§3.5.3). */
    @FunctionalInterface
    public interface OwnerLeaseLiveness {
        CompletionStage<Boolean> live(String ownerId, long leaseGeneration);
    }

    private final ZLinkAdmissionTickets tickets;
    private final Integer globalLimit;
    private final LongSupplier wallClockMs;
    private final OwnerLeaseLiveness liveness;

    private ZLinkAdmissionQueueHost(
            ZLinkAdmissionTickets tickets,
            Integer globalLimit,
            LongSupplier wallClockMs,
            OwnerLeaseLiveness liveness) {
        this.tickets = tickets;
        this.globalLimit = globalLimit;
        this.wallClockMs = wallClockMs;
        this.liveness = liveness;
    }

    /** The host's queue settings, or {@code null} when the admission queue is not enabled. */
    public static ZLinkAdmissionQueueHost create(
            List<StreamNodeRegistration> streamNodes,
            List<MeshNodeRegistration> meshNodes,
            boolean locationStore,
            OwnerLeaseLiveness liveness) {
        if (streamNodes.isEmpty()) {
            return null;
        }
        var queue = ZLinkStreamAuthenticationAdmission.settings(streamNodes, meshNodes).queue();
        if (queue == null) {
            return null;
        }
        if (!locationStore) {
            throw new ZLinkConfigurationException(
                    "The admission queue RouteMesh requires a Location Store.");
        }
        if (meshNodes.stream()
                .noneMatch(
                        mesh ->
                                mesh.meshName().equals(queue.meshName())
                                        && mesh.objectRoleEnabled())) {
            throw new ZLinkConfigurationException(
                    "The admission queue RouteMesh must not have Object role None.");
        }
        return new ZLinkAdmissionQueueHost(
                new ZLinkAdmissionTickets(queue.key(), queue.previousKey()),
                queue.globalLimit(),
                System::currentTimeMillis,
                liveness);
    }

    ZLinkAdmissionQueueState newState(long epoch) {
        return new ZLinkAdmissionQueueState(tickets, globalLimit, wallClockMs, epoch);
    }

    CompletionStage<Boolean> ownerLeaseLive(String ownerId, long leaseGeneration) {
        return liveness == null
                ? CompletableFuture.completedFuture(true)
                : liveness.live(ownerId, leaseGeneration);
    }
}
