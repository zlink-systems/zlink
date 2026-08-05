package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import java.util.HashSet;
import java.util.Set;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlD5MixedBurstScenario {
    private RlD5MixedBurstScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        long firstWindowNanos = 0;
        long lastWindowNanos = 0;
        Set<String> providers = new HashSet<>();
        for (int window = 0; window < 4; window++) {
            long started = System.nanoTime();
            for (int index = 0; index < 40; index++) {
                String value = "d5-soak-" + window + "-" + index;
                if (index % 5 == 0) {
                    context.send(value);
                } else {
                    Contracts.WorkRes reply = context.request(value, Duration.ofSeconds(3));
                    ResilienceScenarioContext.ensure(reply.value().equals("work:" + value),
                        "RL-D5 reply payload mismatch for " + value);
                    providers.add(reply.providerRid());
                }
            }
            long elapsed = System.nanoTime() - started;
            if (window == 0) {
                firstWindowNanos = elapsed;
            }
            lastWindowNanos = elapsed;
        }
        ResilienceScenarioContext.ensure(!providers.isEmpty(),
            "RL-D5 did not observe request replies");
        ResilienceScenarioContext.ensure(lastWindowNanos < firstWindowNanos * 5,
            "RL-D5 latency drift exceeded the harness threshold");
        System.out.println("scenario RL-D5 passed");
    }
}
