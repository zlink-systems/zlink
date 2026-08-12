package systems.zlink.e2e.registrymessaging.provider.Endpoints;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CompletableFuture;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.beans.factory.ObjectProvider;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.registrymessaging.provider.Infrastructure.ScenarioState;
import systems.zlink.e2e.registrymessaging.provider.Infrastructure.ProfileGate;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.e2e.registrymessaging.shared.FailureEvidence;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.e2e.registrymessaging.shared.IdentityOperations;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

@RestController
public final class ProviderEndpoints {
    private final ScenarioState state;
    private final ZLinkClient client;
    private final ZLinkRouteClient routes;
    private final ZLinkFrameworkLifecycle drain;
    private final ObjectProvider<ZLinkActorManager> actors;
    private final ObjectProvider<ZLinkSpotManager> spots;
    private final ObjectProvider<ZLinkActorClient> actorClient;

    public ProviderEndpoints(
        ScenarioState state,
        ZLinkClient client,
        ZLinkRouteClient routes,
        ZLinkFrameworkLifecycle drain,
        ObjectProvider<ZLinkActorManager> actors,
        ObjectProvider<ZLinkSpotManager> spots,
        ObjectProvider<ZLinkActorClient> actorClient) {
        this.state = state;
        this.client = client;
        this.routes = routes;
        this.drain = drain;
        this.actors = actors;
        this.spots = spots;
        this.actorClient = actorClient;
    }

    @GetMapping("/health")
    public Map<String, String> health() {
        return Map.of("status", "ready", "rid", state.providerRid());
    }

    @GetMapping("/route/status")
    public Map<String, Object> routeStatus() {
        var snapshot = drain.routeMeshRuntime().snapshot(Contracts.ROUTE_CHANNEL);
        return Map.of(
            "meshName", snapshot.meshName(),
            "ready", snapshot.isReady(),
            "readyPeerCount", snapshot.readyPeerCount(),
            "peers", snapshot.peers().stream()
                .map(peer -> Map.<String, Object>of(
                    "nodeRid", peer.nodeRid().toString(),
                    "state", peer.state().name().toLowerCase(Locale.ROOT)))
                .toList());
    }

    @GetMapping("/evidence")
    public List<String> evidence() {
        return state.lines();
    }

    @PostMapping("/identity/create")
    public CompletionStage<Contracts.IdentityCreateRes> identityCreate(
        @RequestBody Contracts.IdentityCreateReq request) {
        Contracts.IdentityCreateRes result = IdentityOperations.create(
                state.providerRid(), request, actors.getIfAvailable(), spots.getIfAvailable())
            .toCompletableFuture().join();
        state.record("IdentityCreate", request.marker() + ":" + result.actorState()
            + ":" + result.spotState());
        return CompletableFuture.completedFuture(result);
    }

    @PostMapping("/identity/ping")
    public CompletionStage<Contracts.IdentityPingRes> identityPing(
        @RequestBody Contracts.IdentityPingReq request) {
        return IdentityOperations.ping(
                request, actorClient.getIfAvailable(), routes)
            .thenApply(result -> {
                state.record("IdentityPing", request.marker());
                return result;
            });
    }

    @PostMapping("/identity/ping-actor")
    public CompletionStage<Contracts.IdentityActorPingRes> identityActorPing(
        @RequestBody Contracts.IdentityActorDirectReq request) {
        return IdentityOperations.pingActor(request, actorClient.getIfAvailable())
            .thenApply(result -> {
                state.record("IdentityActorPing", request.marker());
                return result;
            });
    }

    @PostMapping("/identity/ping-spot")
    public CompletionStage<Contracts.IdentitySpotPingRes> identitySpotPing(
        @RequestBody Contracts.IdentitySpotDirectReq request) {
        return IdentityOperations.pingSpot(request, routes)
            .thenApply(result -> {
                state.record("IdentitySpotPing", request.marker());
                return result;
            });
    }

    @PostMapping("/profile/gate/reset")
    public Map<String, String> resetProfileGate(
        ProfileGate gate) {
        gate.reset();
        return Map.of("status", "closed");
    }

    @PostMapping("/profile/gate/release")
    public Map<String, String> releaseProfileGate(
        ProfileGate gate) {
        gate.open();
        return Map.of("status", "released");
    }

    @PostMapping("/evidence/clear")
    public Map<String, String> clearEvidence() {
        state.clear();
        return Map.of("status", "cleared");
    }

    @PostMapping("/admin/drain")
    public CompletionStage<Map<String, String>> drain() {
        return drain.shutdown(Duration.ofSeconds(30))
            .thenApply(result -> Map.of(
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
    public Map<String, String> profileCommand(@RequestBody Contracts.ProfileMsg command) {
        client.sendToChannel(Contracts.API_CHANNEL, command)
            .submit();
        return Map.of("status", "sent");
    }

    @PostMapping("/profile/route/request")
    public CompletionStage<Contracts.RouteRes> routeRequest(@RequestBody Contracts.RouteReq request) {
        return routes.requestToNode(Contracts.ROUTE_CHANNEL, RoutingId.from("api-b"), request)
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.RouteRes.class);
    }

    @PostMapping("/profile/route/payload")
    public CompletionStage<Contracts.PayloadRes> routePayload(
        @RequestBody Contracts.PayloadReq request) {
        return routes.requestToNode(
                Contracts.ROUTE_CHANNEL,
                RoutingId.from("api-b"),
                request)
            .timeout(Duration.ofSeconds(10))
            .submit(Contracts.PayloadRes.class);
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
