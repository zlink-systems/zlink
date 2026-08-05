package systems.zlink.e2e.registrymessaging.provider.Endpoints;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.registrymessaging.provider.Infrastructure.ScenarioState;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.e2e.registrymessaging.shared.FailureEvidence;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

@RestController
public final class ProviderEndpoints {
    private final ScenarioState state;
    private final ZLinkClient client;
    private final ZLinkRouteClient routes;
    private final ZLinkFrameworkLifecycle drain;

    public ProviderEndpoints(
        ScenarioState state,
        ZLinkClient client,
        ZLinkRouteClient routes,
        ZLinkFrameworkLifecycle drain) {
        this.state = state;
        this.client = client;
        this.routes = routes;
        this.drain = drain;
    }

    @GetMapping("/health")
    public java.util.Map<String, String> health() {
        return java.util.Map.of("status", "ready", "rid", state.providerRid());
    }

    @GetMapping("/route/status")
    public java.util.Map<String, Object> routeStatus() {
        var snapshot = drain.routeMeshRuntime().snapshot(Contracts.ROUTE_CHANNEL);
        return java.util.Map.of(
            "meshName", snapshot.meshName(),
            "ready", snapshot.isReady(),
            "readyPeerCount", snapshot.readyPeerCount(),
            "peers", snapshot.peers().stream()
                .map(peer -> java.util.Map.<String, Object>of(
                    "nodeRid", peer.nodeRid().toString(),
                    "state", peer.state().name().toLowerCase(java.util.Locale.ROOT)))
                .toList());
    }

    @GetMapping("/evidence")
    public List<String> evidence() {
        return state.lines();
    }

    @PostMapping("/evidence/clear")
    public java.util.Map<String, String> clearEvidence() {
        state.clear();
        return java.util.Map.of("status", "cleared");
    }

    @PostMapping("/admin/drain")
    public CompletionStage<java.util.Map<String, String>> drain() {
        return drain.shutdown(Duration.ofSeconds(30))
            .thenApply(result -> java.util.Map.of(
                "result", result.outcome() == systems.zlink.framework.runtime.host
                    .ZLinkFrameworkTerminationOutcome.STOPPED ? "Drained" : "ForceStopped"));
    }

    @PostMapping("/evidence/wait")
    public List<String> waitEvidence(@RequestBody Contracts.EvidenceWaitReq request) {
        long timeout = Math.max(1, Math.min(30000, request.timeoutMilliseconds()));
        return state.waitUntil(line -> line.contains(request.contains()), timeout);
    }

    @PostMapping("/profile/request")
    public CompletionStage<Contracts.ProfileRes> profileRequest(@RequestBody Contracts.ProfileReq request) {
        return requestProfile(Contracts.API_CHANNEL, request, Duration.ofSeconds(5));
    }

    @PostMapping("/profile/command")
    public java.util.Map<String, String> profileCommand(@RequestBody Contracts.ProfileMsg command) {
        client.sendToChannel(Contracts.API_CHANNEL, command)
            .submit();
        return java.util.Map.of("status", "sent");
    }

    @PostMapping("/profile/route/request")
    public CompletionStage<Contracts.RouteRes> routeRequest(@RequestBody Contracts.RouteReq request) {
        return routes.requestToNode(Contracts.ROUTE_CHANNEL, RoutingId.from("api-b"), request)
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.RouteRes.class);
    }

    @PostMapping("/profile/route/missing")
    public CompletionStage<Contracts.RequestFailureRes> routeMissing(@RequestBody Contracts.RouteReq request) {
        return routes.requestToNode(Contracts.ROUTE_CHANNEL, RoutingId.from("missing-rid"), request)
                .timeout(Duration.ofMillis(300))
                .submit(Contracts.RouteRes.class)
            .handle((ignored, error) -> FailureEvidence.from(error));
    }

    private CompletionStage<Contracts.ProfileRes> requestProfile(
        String channelName,
        Contracts.ProfileReq request,
        Duration timeout) {
        return client.requestToChannel(channelName, request)
            .timeout(timeout)
            .submit(Contracts.ProfileRes.class);
    }
}
