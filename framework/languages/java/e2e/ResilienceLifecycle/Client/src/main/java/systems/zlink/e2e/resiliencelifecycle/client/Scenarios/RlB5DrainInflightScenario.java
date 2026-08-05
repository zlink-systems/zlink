package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlB5DrainInflightScenario {
    private RlB5DrainInflightScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.post(context.adminB() + "/admin/drain");
        context.waitForWeight(context.adminB(), 0);
        ResilienceScenarioContext.sleep(1500);
        CompletionStage<Contracts.WorkRes> slow =
            context.requestAsync("slow", Duration.ofSeconds(15));
        context.waitForEvidence(context.adminA(), "SlowStarted");
        context.post(context.adminA() + "/admin/drain");
        context.waitForWeight(context.adminA(), 0);
        context.post(context.adminB() + "/admin/restore");
        context.waitForWeight(context.adminB(), 100);
        context.collectStableProvidersWithout("b5-after-drain", "api-a", "api-b");
        context.post(context.adminA() + "/admin/release-slow");
        Contracts.WorkRes slowReply;
        try {
            slowReply = slow.toCompletableFuture().get(20, TimeUnit.SECONDS);
        } catch (Exception error) {
            throw new IllegalStateException("RL-B5 slow request did not complete", error);
        }
        ResilienceScenarioContext.ensure("api-a".equals(slowReply.providerRid()),
            "RL-B5 slow request was not served by api-a: " + slowReply.providerRid());
        ResilienceScenarioContext.ensure("work:slow".equals(slowReply.value()),
            "RL-B5 slow reply payload mismatch");
        context.post(context.adminA() + "/admin/restore");
        context.waitForWeight(context.adminA(), 100);
        System.out.println("scenario RL-B5 passed");
    }
}
