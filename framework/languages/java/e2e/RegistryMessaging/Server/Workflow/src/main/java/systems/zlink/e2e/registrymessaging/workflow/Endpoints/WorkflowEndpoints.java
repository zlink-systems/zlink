package systems.zlink.e2e.registrymessaging.workflow.Endpoints;
import java.util.Map;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.beans.factory.ObjectProvider;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.e2e.registrymessaging.workflow.Infrastructure.ScenarioState;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.e2e.registrymessaging.shared.IdentityOperations;

@RestController
public final class WorkflowEndpoints {
    private final ScenarioState state;
    private final ZLinkClient client;
    private final ZLinkRouteClient routes;
    private final ObjectProvider<ZLinkActorManager> actors;
    private final ObjectProvider<ZLinkSpotManager> spots;
    private final ObjectProvider<ZLinkActorClient> actorClient;

    public WorkflowEndpoints(
        ScenarioState state,
        ZLinkClient client,
        ZLinkRouteClient routes,
        ObjectProvider<ZLinkActorManager> actors,
        ObjectProvider<ZLinkSpotManager> spots,
        ObjectProvider<ZLinkActorClient> actorClient) {
        this.state = state;
        this.client = client;
        this.routes = routes;
        this.actors = actors;
        this.spots = spots;
        this.actorClient = actorClient;
    }

    @GetMapping("/health")
    public Map<String, String> health() {
        return Map.of("status", "ready", "rid", state.providerRid());
    }

    @GetMapping("/evidence")
    public List<String> evidence() {
        return state.lines();
    }

    @PostMapping("/identity/create")
    public CompletionStage<Contracts.IdentityCreateRes> identityCreate(
        @RequestBody Contracts.IdentityCreateReq request) {
        return IdentityOperations.create(
                state.providerRid(), request, actors.getIfAvailable(), spots.getIfAvailable())
            .thenApply(result -> {
                state.record("IdentityCreate", request.marker() + ":" + result.actorState()
                    + ":" + result.spotState());
                return result;
            });
    }

    @PostMapping("/identity/ping")
    public CompletionStage<Contracts.IdentityPingRes> identityPing(
        @RequestBody Contracts.IdentityPingReq request) {
        return IdentityOperations.ping(request, actorClient.getIfAvailable(), routes)
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

    @PostMapping("/evidence/clear")
    public Map<String, String> clearEvidence() {
        state.clear();
        return Map.of("status", "cleared");
    }

    @PostMapping("/evidence/wait")
    public List<String> waitEvidence(@RequestBody Contracts.EvidenceWaitReq request) {
        long timeout = Math.max(1, Math.min(30000, request.timeoutMilliseconds()));
        return state.waitUntil(line -> line.contains(request.contains()), timeout);
    }

    @PostMapping("/workflow/request")
    public CompletionStage<Contracts.WorkflowRes> workflowRequest(@RequestBody Contracts.WorkflowReq request) {
        return client.requestToChannel(Contracts.WORKFLOW_CHANNEL, request)
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.WorkflowRes.class);
    }
}
