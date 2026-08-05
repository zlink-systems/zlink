package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import java.util.HashSet;
import java.util.Set;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlA5ProviderFlappingScenario {
    private RlA5ProviderFlappingScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        context.signal("a5-ready");
        Set<String> providers = new HashSet<>();
        int successes = 0;
        int failures = 0;
        int index = 0;
        while (!context.hasSignal("a5-stop")) {
            try {
                Contracts.WorkRes reply = context.request(
                    "a5-flap-" + index, Duration.ofSeconds(3));
                ResilienceScenarioContext.ensure(reply.value().equals("work:a5-flap-" + index),
                    "RL-A5 reply payload mismatch");
                providers.add(reply.providerRid());
                successes++;
            } catch (RuntimeException error) {
                failures++;
                ResilienceScenarioContext.ensure(failures <= 5,
                    "RL-A5 observed repeated failures during provider flapping");
            }
            index++;
            ResilienceScenarioContext.sleep(100);
        }
        context.waitForTopology(2);
        Contracts.WorkRes followUp = context.request("a5-follow-up", Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure("work:a5-follow-up".equals(followUp.value()),
            "RL-A5 follow-up payload mismatch");
        ResilienceScenarioContext.ensure(successes >= 10,
            "RL-A5 did not send enough traffic during flapping");
        ResilienceScenarioContext.ensure(providers.contains("api-b"),
            "RL-A5 did not converge to live api-b during flapping");
        System.out.println("scenario RL-A5 passed");
    }
}
