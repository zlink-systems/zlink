package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlD3DispatchErrorEvidenceScenario {
    private RlD3DispatchErrorEvidenceScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        try {
            context.requestUnhandled("d3-missing-handler", Duration.ofSeconds(3));
            throw new IllegalStateException("RL-D3 missing handler request unexpectedly completed");
        } catch (RuntimeException expected) {
            context.waitForDispatchErrorAny("UnhandledReq", context.adminA(), context.adminB());
        }
        Contracts.WorkRes followUp = context.request("d3-follow-up", Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure("work:d3-follow-up".equals(followUp.value()),
            "RL-D3 follow-up payload mismatch");
        System.out.println("scenario RL-D3 passed");
    }
}
