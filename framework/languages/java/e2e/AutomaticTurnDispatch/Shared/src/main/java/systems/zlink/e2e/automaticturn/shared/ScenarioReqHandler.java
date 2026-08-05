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

public final class ScenarioReqHandler
    implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, Contracts.ScenarioReq> {
    private static final Duration ROUTE_REQUEST_TIMEOUT = Duration.ofSeconds(30);

    private final ZLinkRouteClient routes;
    private final SpotHandleResolver spots;
    private final EvidenceStore evidence;

    public ScenarioReqHandler(
        ZLinkRouteClient routes,
        SpotHandleResolver spots,
        EvidenceStore evidence) {
        this.routes = routes;
        this.spots = spots;
        this.evidence = evidence;
    }

    @Override
    public Class<Contracts.ScenarioReq> messageType() {
        return Contracts.ScenarioReq.class;
    }

    @Override
    public CompletionStage<Void> handle(
        ZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        Contracts.ScenarioReq request) {
        evidence.record("scenario-started", request.scenarioId(), request.requestId());
        Object spotRequest = switch (request.scenarioId()) {
            case "ATD-A1" -> new Contracts.HoldReq(request.requestId());
            case "ATD-A2" -> new Contracts.AwaitReq(request.scenarioId(), request.requestId(), "corr-a2");
            case "ATD-A3" -> new Contracts.AwaitReq(request.scenarioId(), request.requestId(), "corr-a3");
            case "ATD-A4" -> new Contracts.WorkerAwaitReq(request.requestId());
            case "OBS-B2-QUEUE" -> new Contracts.ObservabilityQueueReq(request.requestId());
            default -> throw new IllegalArgumentException("unknown scenario " + request.scenarioId());
        };
        RoutingId targetSpotRid = RoutingId.from(dispatch.metadata()
            .getOrDefault(Contracts.SPOT_RID_METADATA, Contracts.TARGET_SPOT));
        return spots.resolveSpotHandle(targetSpotRid)
            .thenCompose(handle -> runScenario(
                requireSpot(handle, targetSpotRid),
                spotRequest,
                request))
            .whenComplete((reply, error) -> {
                if (error != null) {
                    evidence.record("scenario-failed", request.scenarioId(), error.toString());
                }
            })
            .thenAccept(reply -> context.client().reply(reply).submit());
    }

    private CompletionStage<Contracts.ScenarioRes> runScenario(
        SpotHandle handle,
        Object spotRequest,
        Contracts.ScenarioReq request) {
        CompletionStage<Contracts.ScenarioRes> scenario = routes.requestToSpot(
                handle,
                spotRequest)
            .timeout(ROUTE_REQUEST_TIMEOUT)
            .submit(Contracts.ScenarioRes.class);
        if (!("ATD-A1".equals(request.scenarioId())
            || "ATD-A2".equals(request.scenarioId())
            || "ATD-A4".equals(request.scenarioId()))) {
            return scenario;
        }
        CompletionStage<Contracts.ProbeRes> probe = delayed(500)
            .thenCompose(ignored -> routes.requestToSpot(
                    handle,
                    new Contracts.ProbeReq(request.requestId()))
                .timeout(ROUTE_REQUEST_TIMEOUT)
                .submit(Contracts.ProbeRes.class));
        return "ATD-A1".equals(request.scenarioId())
            ? scenario.thenCompose(reply -> probe.thenApply(ignored -> reply))
            : scenario.thenCombine(probe, (reply, ignored) -> reply);
    }

    private static CompletionStage<Void> delayed(long millis) {
        return CompletableFuture.runAsync(
            () -> { },
            CompletableFuture.delayedExecutor(millis, TimeUnit.MILLISECONDS));
    }

    private static SpotHandle requireSpot(
        java.util.Optional<SpotHandle> handle,
        RoutingId spotRid) {
        return handle.orElseThrow(() -> new IllegalStateException("spot not found: " + spotRid));
    }
}
