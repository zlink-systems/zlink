package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlB2CrashDuringInflightScenario {
    private RlB2CrashDuringInflightScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        context.post(context.adminA() + "/admin/drain");
        context.waitForWeight(context.adminA(), 0);
        context.collectStableProvidersWithout("b2-before-crash", "api-a", "api-b");
        CompletionStage<Contracts.WorkRes> slow =
            context.requestAsync("slow", Duration.ofSeconds(12));
        context.waitForEvidence(context.adminB(), "SlowStarted");
        context.signal("b2-in-flight");
        context.waitForSignal("b2-crashed");
        boolean failed = false;
        try {
            slow.toCompletableFuture().get(15, TimeUnit.SECONDS);
        } catch (Exception expected) {
            failed = true;
        }
        ResilienceScenarioContext.ensure(failed,
            "RL-B2 in-flight request unexpectedly completed after provider crash");
        context.post(context.adminA() + "/admin/restore");
        context.waitForWeight(context.adminA(), 100);
        context.waitForTopologyWithout("api-b", 30);
        context.collectStableProvidersWithoutFailures("b2-after-crash", "api-b", "api-a");
        context.signal("b2-survivor-observed");
        context.waitForSignal("b2-restored");
        context.waitForTopologyEndpoint("api-b", context.options().apiBEndpoint());
        context.driveUntilEvidence(
            context.adminB(), "b2-restored", "RL-B2 restored provider traffic missing");
        System.out.println("scenario RL-B2 passed");
    }
}
