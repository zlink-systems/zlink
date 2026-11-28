package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermit;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.AdmitPermitReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.Enqueue;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.EnqueueReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReport;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.NodeReportReply;
import systems.zlink.framework.runtime.streams.ZLinkAdmissionQueueMessages.ReleasePermit;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/**
 * The login admission queue Instance Spot (Authentication Admission §3.5.1): one per RouteMesh,
 * placed by the existing Instance Spot rules. Its memory is {@link ZLinkAdmissionQueueState}; a new
 * incarnation starts with a new epoch and no waiters.
 */
public final class ZLinkAdmissionQueueSpot implements ZLinkInstanceSpot {
    private final ZLinkInstanceSpotContext context;
    private final ZLinkAdmissionQueueHost host;
    private final ZLinkAdmissionQueueState state;

    public ZLinkAdmissionQueueSpot(ZLinkInstanceSpotContext context, ZLinkAdmissionQueueHost host) {
        this.context = context;
        this.host = host;
        this.state = host.newState(ZLinkAdmissionQueueState.newEpoch());
    }

    @Override
    public ZLinkInstanceSpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addPacket(ReportHandler.class);
        context.handlers().addPacket(EnqueueHandler.class);
        context.handlers().addPacket(AdmitPermitHandler.class);
        context.handlers().addPacket(ReleasePermitHandler.class);
    }

    /** Reclaims permits whose node owner lease ended before a permit is refused (§3.5.3). */
    private CompletionStage<Void> reclaimEndedLeases(Collection<Map.Entry<String, Long>> owners) {
        if (owners.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        var checkedOwners = List.copyOf(owners);
        var checks =
                checkedOwners.stream()
                        .map(
                                owner ->
                                        host.ownerLeaseLive(owner.getKey(), owner.getValue())
                                                .toCompletableFuture())
                        .toList();
        return CompletableFuture.allOf(checks.toArray(CompletableFuture[]::new))
                .thenRun(
                        () -> {
                            for (int i = 0; i < checkedOwners.size(); i++) {
                                if (Boolean.FALSE.equals(checks.get(i).join())) {
                                    var owner = checkedOwners.get(i);
                                    state.ownerLeaseEnded(owner.getKey(), owner.getValue());
                                }
                            }
                        });
    }

    public static final class ReportHandler
            implements ZLinkSpotRequestHandler<
                    ZLinkAdmissionQueueSpot, NodeReport, NodeReportReply> {
        @Override
        public CompletionStage<NodeReportReply> handle(
                ZLinkAdmissionQueueSpot spot, NodeReport request) {
            return spot.state.report(request, spot::reclaimEndedLeases);
        }
    }

    public static final class EnqueueHandler
            implements ZLinkSpotRequestHandler<ZLinkAdmissionQueueSpot, Enqueue, EnqueueReply> {
        @Override
        public CompletionStage<EnqueueReply> handle(ZLinkAdmissionQueueSpot spot, Enqueue request) {
            return CompletableFuture.completedFuture(spot.state.enqueue(request.connection()));
        }
    }

    public static final class AdmitPermitHandler
            implements ZLinkSpotRequestHandler<
                    ZLinkAdmissionQueueSpot, AdmitPermit, AdmitPermitReply> {
        @Override
        public CompletionStage<AdmitPermitReply> handle(
                ZLinkAdmissionQueueSpot spot, AdmitPermit request) {
            return spot.reclaimEndedLeases(spot.state.ownersToCheckForAdmit())
                    .thenApply(
                            ignored -> {
                                var result = spot.state.admitPermit(request);
                                return result.granted()
                                        ? new AdmitPermitReply(true, result.permitId())
                                        : new AdmitPermitReply(false, null);
                            });
        }
    }

    public static final class ReleasePermitHandler
            implements ZLinkSpotPacketHandler<ZLinkAdmissionQueueSpot, ReleasePermit> {
        @Override
        public CompletionStage<Void> handle(ZLinkAdmissionQueueSpot spot, ReleasePermit message) {
            spot.state.releasePermit(message.permitId());
            return CompletableFuture.completedFuture(null);
        }
    }
}
