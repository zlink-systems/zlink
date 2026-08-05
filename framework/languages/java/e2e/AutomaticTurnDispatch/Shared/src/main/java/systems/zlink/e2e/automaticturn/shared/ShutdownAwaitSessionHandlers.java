package systems.zlink.e2e.automaticturn.shared;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

public final class ShutdownAwaitSessionHandlers {
    private static final Duration ROUTE_REQUEST_TIMEOUT = Duration.ofSeconds(90);
    private static final Duration RECOVERY_REQUEST_TIMEOUT = Duration.ofSeconds(30);
    private static final Duration RECOVERY_PROBE_ATTEMPT_TIMEOUT = Duration.ofSeconds(5);

    private ShutdownAwaitSessionHandlers() {
    }

    public static final class Wait
        implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.AwaitShutdownScenarioReq> {
        private final ZLinkRouteClient routes;
        private final SpotHandleResolver spots;

        public Wait(ZLinkRouteClient routes, SpotHandleResolver spots) {
            this.routes = routes;
            this.spots = spots;
        }

        @Override
        public Class<Contracts.AwaitShutdownScenarioReq> messageType() {
            return Contracts.AwaitShutdownScenarioReq.class;
        }

        @Override
        public CompletionStage<Void> handle(
            ZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            Contracts.AwaitShutdownScenarioReq request) {
            RoutingId playNode = RoutingId.from(Contracts.PLAY_NODE_A);
            RoutingId spotRid = RoutingId.from(request.spotRid());
            return ensure(routes, playNode, request.spotRid())
                .thenCompose(ignored -> spots.resolveSpotHandle(spotRid))
                .thenCompose(handle -> routes.requestToSpot(
                        requireSpot(handle, spotRid),
                        new Contracts.AwaitReq("ATD-E3", request.requestId(), "shutdown"))
                    .timeout(ROUTE_REQUEST_TIMEOUT)
                    .submit(Contracts.ScenarioRes.class))
                .thenAccept(ignored -> context.client().reply(new Contracts.AwaitShutdownRes(
                    "atd.e3-shutdown-unexpected-completion",
                    request.requestId(),
                    request.spotRid())).submit());
        }
    }

    public static final class Recovery
        implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.AwaitShutdownRecoveryReq> {
        private final ZLinkRouteClient routes;
        private final SpotHandleResolver spots;

        public Recovery(ZLinkRouteClient routes, SpotHandleResolver spots) {
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
            RoutingId playNode = RoutingId.from(Contracts.PLAY_NODE_A);
            RoutingId spotRid = RoutingId.from(request.spotRid());
            return ensure(routes, playNode, request.spotRid())
                .thenCompose(ignored -> probe(routes, spots, spotRid, request.requestId(), 5))
                .thenRun(() -> context.client().reply(new Contracts.AwaitShutdownRes(
                    "atd.e3-shutdown-recovery",
                    request.requestId(),
                    request.spotRid())).submit());
        }
    }

    private static CompletionStage<Contracts.EnsureSpotRes> ensure(
        ZLinkRouteClient routes,
        RoutingId nodeRid,
        String spotRid) {
        return routes.requestToNode(
                Contracts.ROUTE_CHANNEL,
                nodeRid,
                new Contracts.EnsureSpotReq(spotRid))
            .timeout(RECOVERY_REQUEST_TIMEOUT)
            .submit(Contracts.EnsureSpotRes.class);
    }

    private static CompletionStage<Void> probe(
        ZLinkRouteClient routes,
        SpotHandleResolver spots,
        RoutingId spotRid,
        String requestId,
        int remaining) {
        return spots.resolveSpotHandle(spotRid)
            .thenCompose(handle -> routes.requestToSpot(
                    requireSpot(handle, spotRid),
                    new Contracts.ProbeReq(requestId))
                .timeout(RECOVERY_PROBE_ATTEMPT_TIMEOUT)
                .submit(Contracts.ProbeRes.class))
            .handle((reply, error) -> {
                if (error == null) {
                    return CompletableFuture.<Void>completedFuture(null);
                }
                if (remaining <= 1) {
                    return CompletableFuture.<Void>failedFuture(error);
                }
                return CompletableFuture.runAsync(
                        () -> { },
                        CompletableFuture.delayedExecutor(500, TimeUnit.MILLISECONDS))
                    .thenCompose(ignored -> probe(routes, spots, spotRid, requestId, remaining - 1));
            })
            .thenCompose(stage -> stage);
    }

    private static SpotHandle requireSpot(java.util.Optional<SpotHandle> handle, RoutingId spotRid) {
        return handle.orElseThrow(() -> new IllegalStateException("spot not found: " + spotRid));
    }
}
