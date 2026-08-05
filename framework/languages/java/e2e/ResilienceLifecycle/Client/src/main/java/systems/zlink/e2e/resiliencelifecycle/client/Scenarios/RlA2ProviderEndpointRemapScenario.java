package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import java.util.concurrent.TimeUnit;

public final class RlA2ProviderEndpointRemapScenario {
    private RlA2ProviderEndpointRemapScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        context.post(context.adminB() + "/admin/drain");
        context.waitForWeight(context.adminB(), 0);
        context.collectStableProvidersWithout("a2-before-reschedule", "api-b", "api-a");
        long oldGeneration = context.peerGeneration("api-a", context.options().apiAEndpoint());
        var inFlight = context.requestAsync("slow", Duration.ofSeconds(15));
        context.waitForEvidence(context.adminA(), "SlowStarted");
        context.signal("a2-ready");
        context.waitForSignal("a2-down");
        try {
            inFlight.toCompletableFuture().get(20, TimeUnit.SECONDS);
            throw new IllegalStateException("RL-A2 crashed in-flight request unexpectedly completed");
        } catch (java.util.concurrent.ExecutionException expected) {
            // The crash contract requires a bounded public failure, not a successful reply.
        } catch (Exception error) {
            throw new IllegalStateException("RL-A2 crashed in-flight request did not fail in time", error);
        }
        context.waitForTopologyWithout("api-a", 10);
        context.signal("a2-down-observed");
        context.waitForSignal("a2-up");
        context.waitForReplacementGeneration(
            "api-a", context.options().apiAReplacementEndpoint(), oldGeneration);
        for (int index = 0; index < 20; index++) {
            String value = "a2-after-replacement-" + index;
            var reply = context.request(value, Duration.ofSeconds(3));
            ResilienceScenarioContext.ensure("api-a".equals(reply.providerRid()),
                "RL-A2 replacement request reached " + reply.providerRid());
            ResilienceScenarioContext.ensure(
                context.hasEvidence(context.options().httpAReplacementEndpoint(), "WorkReq", value),
                "RL-A2 replacement evidence missing for " + value);
        }
        context.post(context.adminB() + "/admin/restore");
        context.waitForWeight(context.adminB(), 100);
        System.out.println("scenario RL-A2 passed");
    }
}
