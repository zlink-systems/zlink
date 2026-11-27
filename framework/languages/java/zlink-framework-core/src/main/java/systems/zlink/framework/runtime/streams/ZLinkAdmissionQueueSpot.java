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

import java.util.ArrayList;
import java.util.List;
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
    private CompletionStage<Void> reclaimEndedLeases() {
        if (!host.atLimit(state.runningPermits())) {
            return CompletableFuture.completedFuture(null);
        }
        List<CompletableFuture<Void>> checks = new ArrayList<>();
        for (var owner : state.permitOwners()) {
            checks.add(
                    host.ownerLeaseLive(owner.getKey(), owner.getValue())
                            .handle(
                                    (live, failure) -> {
                                        if (failure == null && Boolean.FALSE.equals(live)) {
                                            state.ownerLeaseEnded(owner.getKey(), owner.getValue());
                                        }
                                        return (Void) null;
                                    })
                            .toCompletableFuture());
        }
        return CompletableFuture.allOf(checks.toArray(CompletableFuture[]::new));
    }

    public static final class ReportHandler
            implements ZLinkSpotRequestHandler<
                    ZLinkAdmissionQueueSpot, NodeReport, NodeReportReply> {
        @Override
        public CompletionStage<NodeReportReply> handle(
                ZLinkAdmissionQueueSpot spot, NodeReport request) {
            return CompletableFuture.completedFuture(spot.state.report(request));
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
            return spot.reclaimEndedLeases()
                    .thenCompose(
                            ignored -> {
                                var result = spot.state.admitPermit(request);
                                return CompletableFuture.completedFuture(
                                        result.granted()
                                                ? new AdmitPermitReply(true, result.permitId())
                                                : new AdmitPermitReply(false, null));
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
