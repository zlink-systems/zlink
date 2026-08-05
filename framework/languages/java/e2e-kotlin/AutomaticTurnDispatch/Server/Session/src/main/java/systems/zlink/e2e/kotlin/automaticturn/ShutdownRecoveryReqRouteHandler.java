package systems.zlink.e2e.kotlin.automaticturn;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class ShutdownRecoveryReqRouteHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.AwaitShutdownRecoveryReq> {
    private static final Duration RECOVERY_DEADLINE = Duration.ofSeconds(90);
    private static final Duration ATTEMPT_TIMEOUT = Duration.ofSeconds(15);

    private final ZLinkRouteClient routes;
    private final SpotHandleResolver spots;

    public ShutdownRecoveryReqRouteHandler(ZLinkRouteClient routes, SpotHandleResolver spots) {
        this.routes = routes;
        this.spots = spots;
    }

    @Override
    public Class<Contracts.AwaitShutdownRecoveryReq> messageType() {
        return Contracts.AwaitShutdownRecoveryReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.AwaitShutdownRecoveryReq request) {
        RoutingId targetNode = SpotMsgRouteHandler.targetNode(dispatch);
        RoutingId targetSpot = RoutingId.from(request.spotRid());
        long deadline = System.nanoTime() + RECOVERY_DEADLINE.toNanos();
        return ensureSpot(targetNode, request.spotRid())
            .thenCompose(ignored -> requestRecoveryProbe(targetSpot, request, deadline))
            .thenAccept(evidence -> context.client()
                .reply(new Contracts.AwaitShutdownRecoveryRes(
                    "await.e3-shutdown-recovery",
                    request.spotRid(),
                    evidence.markers()))
                .submit());
    }

    private CompletionStage<Contracts.EnsureSpotRes> ensureSpot(
        RoutingId targetNode,
        String spotRid) {
        return routes.requestToNode(
                Contracts.SPOT_MESH,
                targetNode,
                new Contracts.EnsureSpotReq(spotRid))
            .timeout(ATTEMPT_TIMEOUT)
            .submit(Contracts.EnsureSpotRes.class);
    }

    private CompletionStage<Contracts.EvidenceRes> requestRecoveryProbe(
        RoutingId targetSpot,
        Contracts.AwaitShutdownRecoveryReq request,
        long deadline) {
        return spots.resolveSpotHandle(targetSpot)
            .thenCompose(handle -> routes.requestToSpot(
                    handle.orElseThrow(() -> new IllegalStateException("spot not found: " + targetSpot)),
                    new Contracts.ProbeReq("shutdown-recovery-probe", 0))
                .timeout(ATTEMPT_TIMEOUT)
                .submit(Contracts.ProbeRes.class))
            .thenApply(reply -> new Contracts.EvidenceRes(
                request.requestId(),
                List.of("probe-completed|spot=" + reply.spotRid()
                    + ";node=" + reply.nodeRid()
                    + ";marker=shutdown-recovery-probe"
                    + ";value=" + reply.value())))
            .handle((reply, error) -> {
                if (error == null) {
                    return CompletableFuture.completedFuture(reply);
                }
                if (System.nanoTime() >= deadline) {
                    return CompletableFuture.<Contracts.EvidenceRes>failedFuture(
                        new IllegalStateException(
                            "ATD-E3 recovery did not route to restarted play node",
                            error));
                }
                return CompletableFuture.runAsync(
                        () -> { },
                        CompletableFuture.delayedExecutor(500, TimeUnit.MILLISECONDS))
                    .thenCompose(ignored -> requestRecoveryProbe(targetSpot, request, deadline));
            })
            .thenCompose(stage -> stage);
    }
}
