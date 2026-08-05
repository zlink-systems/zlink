package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlC4RegistryOutageScenario {
    private RlC4RegistryOutageScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        Contracts.WorkRes before = context.request("c4-before-outage", Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure("work:c4-before-outage".equals(before.value()),
            "RL-C4 pre-outage reply payload mismatch");
        context.waitForProviderEvidence("c4-before-outage");
        context.signal("c4-pause-ready");
        context.waitForSignal("c4-store-paused");
        Contracts.WorkRes during = context.request("c4-during-outage", Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure("work:c4-during-outage".equals(during.value()),
            "RL-C4 established channel failed during store outage");
        context.waitForProviderEvidence("c4-during-outage");
        context.signal("c4-during-observed");
        context.waitForSignal("c4-store-resumed");
        context.waitForTopology(2);
        Contracts.WorkRes after = context.request("c4-after-recovery", Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure("work:c4-after-recovery".equals(after.value()),
            "RL-C4 recovery reply payload mismatch");
        context.waitForProviderEvidence("c4-after-recovery");
        System.out.println("scenario RL-C4 passed");
    }
}
