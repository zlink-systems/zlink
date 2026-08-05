package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.time.Duration;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class RlD2ObserverFaultScenario {
    private RlD2ObserverFaultScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.post(context.adminA() + "/admin/fault/observer-throws");
        context.post(context.adminB() + "/admin/fault/observer-throws");
        try {
            context.requestUnhandled("d2-observer-fault", Duration.ofSeconds(3));
            throw new IllegalStateException("RL-D2 missing handler request unexpectedly completed");
        } catch (RuntimeException expected) {
            // Observer failures are reported through structured logging. They
            // do not create an application callback or evidence DTO.
        } finally {
            context.post(context.adminA() + "/admin/fault/none");
            context.post(context.adminB() + "/admin/fault/none");
        }
        Contracts.WorkRes followUp = context.request("rl-d2-after", Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure("work:rl-d2-after".equals(followUp.value()),
            "RL-D2 messaging did not continue after observer failure");
        context.waitForEvidenceAny("WorkReq", context.adminA(), context.adminB());
        System.out.println("scenario RL-D2 passed");
    }
}
