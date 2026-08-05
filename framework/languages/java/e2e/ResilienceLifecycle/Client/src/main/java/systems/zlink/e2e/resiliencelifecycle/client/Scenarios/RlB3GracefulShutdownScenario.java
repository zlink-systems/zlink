package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlB3GracefulShutdownScenario {
    private RlB3GracefulShutdownScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        Contracts.WorkRes before = context.request("b3-before-shutdown", Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure("work:b3-before-shutdown".equals(before.value()),
            "RL-B3 pre-shutdown reply payload mismatch");
        context.post(context.adminA() + "/admin/drain");
        context.waitForWeight(context.adminA(), 0);
        ResilienceScenarioContext.sleep(1_500);
        var slow = context.requestAsync("slow", Duration.ofSeconds(15));
        context.waitForEvidence(context.adminB(), "SlowStarted");
        var shutdownRequest = context.postJsonAsync(context.adminB() + "/admin/shutdown");
        context.post(context.adminB() + "/admin/release-slow");
        Contracts.WorkRes completed;
        try {
            completed = slow.toCompletableFuture().get(20, TimeUnit.SECONDS);
        } catch (Exception error) {
            throw new IllegalStateException("RL-B3 in-flight request did not complete", error);
        }
        ResilienceScenarioContext.ensure("api-b".equals(completed.providerRid()),
            "RL-B3 in-flight reply came from an unexpected provider: " + completed.providerRid());
        ResilienceScenarioContext.ensure("work:slow".equals(completed.value()),
            "RL-B3 in-flight reply payload mismatch");
        var shutdown = shutdownRequest.toCompletableFuture().join();
        ResilienceScenarioContext.ensure("Drained".equals(shutdown.path("result").asText()),
            "RL-B3 shutdown did not terminate with Drained: " + shutdown);
        context.post(context.adminA() + "/admin/restore");
        context.waitForWeight(context.adminA(), 100);
        context.waitForTopologyWithout("api-b", 30);
        Set<String> providers = context.collectStableProvidersWithoutFailures(
            "b3-after-shutdown", "api-b", "api-a");
        ResilienceScenarioContext.ensure(providers.contains("api-a"),
            "RL-B3 did not converge to api-a after api-b shutdown");
        System.out.println("scenario RL-B3 passed");
    }
}
