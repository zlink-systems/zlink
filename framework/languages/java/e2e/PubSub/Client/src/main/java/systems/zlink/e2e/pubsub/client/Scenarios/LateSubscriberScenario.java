package systems.zlink.e2e.pubsub.client.Scenarios;

import systems.zlink.e2e.pubsub.client.Support.ScenarioAssert;
import systems.zlink.e2e.pubsub.client.Support.ScenarioContext;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class LateSubscriberScenario {
    private LateSubscriberScenario() {
    }

    public static void run(ScenarioContext context) {
        Contracts.EvidenceSnapshot late = context.evidence().snapshot("sub-3");
        ScenarioAssert.ensure(
            !ScenarioAssert.hasEvent(late, "prelate", 0),
            "PS-A3 late subscriber received replayed pre-late event");
        context.publisher().publish("all", new Contracts.Event("ps-a3", 1, "after-late"));
        ScenarioAssert.waitForEvent(context.evidence(), "sub-3", "ps-a3", 1);
        System.out.println("scenario PS-A3 passed");
    }
}
