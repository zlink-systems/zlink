package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import java.util.HashSet;
import java.util.Set;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlB6GrayFaultScenario {
    private RlB6GrayFaultScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.post(context.adminA() + "/admin/fault-on");
        context.waitForEvidence(context.adminA(), "GrayFailureMode");
        int successes = 0;
        int failures = 0;
        Set<String> providers = new HashSet<>();
        for (int index = 0; index < 80 && successes < 10; index++) {
            try {
                Contracts.WorkRes reply = context.request(
                    "b6-gray-" + index, Duration.ofSeconds(3));
                ResilienceScenarioContext.ensure(reply.value().equals("work:b6-gray-" + index),
                    "RL-B6 reply payload mismatch");
                providers.add(reply.providerRid());
                successes++;
            } catch (RuntimeException expected) {
                failures++;
            }
        }
        context.post(context.adminA() + "/admin/fault-off");
        context.waitForEvidence(context.adminA(), "GrayFailureInjected");
        ResilienceScenarioContext.ensure(failures > 0,
            "RL-B6 did not observe public failures from degraded provider");
        ResilienceScenarioContext.ensure(successes >= 10,
            "RL-B6 healthy provider did not maintain enough successful traffic");
        ResilienceScenarioContext.ensure(providers.contains("api-b"),
            "RL-B6 did not receive successful replies from api-b");
        Contracts.WorkRes followUp = context.request("b6-follow-up", Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure("work:b6-follow-up".equals(followUp.value()),
            "RL-B6 follow-up payload mismatch");
        System.out.println("scenario RL-B6 passed");
    }
}
