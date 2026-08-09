package Scenarios;

import systems.zlink.e2e.pubsub.client.Scenarios;
import systems.zlink.e2e.pubsub.client.Support;
import systems.zlink.e2e.pubsub.client.Support.ScenarioAssert;
import systems.zlink.e2e.pubsub.client.Support.ScenarioContext;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class MissingMessageNameScenario {
    private MissingMessageNameScenario() {
    }

    public static void run(ScenarioContext context) {
        context.publisher().publishMissing(
            "all",
            new Contracts.EventMsg("ps-c1", 1, "missing-packet"));
        ScenarioAssert.waitForDispatchError(context.evidence(), "sub-1", Contracts.MISSING_PACKET);
        context.publisher().publish("all", new Contracts.EventMsg("ps-c1", 2, "normal-after-missing"));
        ScenarioAssert.waitForEvent(context.evidence(), "sub-1", "ps-c1", 2);
        ScenarioAssert.waitForEvent(context.evidence(), "sub-2", "ps-c1", 2);
        ScenarioAssert.waitForEvent(context.evidence(), "sub-3", "ps-c1", 2);
        System.out.println("scenario PS-C1 passed");
    }
}
