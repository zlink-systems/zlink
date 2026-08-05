package systems.zlink.e2e.pubsub.client.Scenarios;

import systems.zlink.e2e.pubsub.client.Support.ScenarioAssert;
import systems.zlink.e2e.pubsub.client.Support.ScenarioContext;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class SlowSubscriberScenario {
    private SlowSubscriberScenario() {
    }

    public static void run(ScenarioContext context) {
        for (int sequence = 1; sequence <= 32; sequence++) {
            context.publisher().publish("all", new Contracts.EventMsg("ps-b1", sequence, "slow-isolation-" + sequence));
        }
        ScenarioAssert.waitForSequenceAtLeast(context.evidence(), "sub-2", "ps-b1", 32);
        ScenarioAssert.waitForSequenceAtLeast(context.evidence(), "sub-3", "ps-b1", 32);
        System.out.println("scenario PS-B1 passed");
    }
}
