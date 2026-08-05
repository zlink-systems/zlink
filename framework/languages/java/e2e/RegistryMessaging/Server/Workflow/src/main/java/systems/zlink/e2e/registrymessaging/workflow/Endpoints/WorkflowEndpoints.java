package systems.zlink.e2e.registrymessaging.workflow.Endpoints;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.e2e.registrymessaging.workflow.Infrastructure.ScenarioState;
import systems.zlink.framework.channels.ZLinkClient;

@RestController
public final class WorkflowEndpoints {
    private final ScenarioState state;
    private final ZLinkClient client;

    public WorkflowEndpoints(ScenarioState state, ZLinkClient client) {
        this.state = state;
        this.client = client;
    }

    @GetMapping("/health")
    public java.util.Map<String, String> health() {
        return java.util.Map.of("status", "ready", "rid", state.providerRid());
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
