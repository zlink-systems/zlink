package systems.zlink.e2e.pubsub.client.Scenarios;

import systems.zlink.e2e.pubsub.client.Support.ScenarioAssert;
import systems.zlink.e2e.pubsub.client.Support.ScenarioContext;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class SubscriberReconnectScenario {
    private SubscriberReconnectScenario() {
    }

    public static void run(ScenarioContext context) {
        try (var subscriber = context.processes().startSubscriber(
            "sub-1",
            "alpha",
            context.options().sub1Http())) {
            context.publisher().publish("all", new Contracts.Event("ps-a4-before", 1, "before-disconnect"));
            ScenarioAssert.waitForEvent(context.evidence(), "sub-1", "ps-a4-before", 1);
        }

        context.processes().waitStopped("sub-1", context.options().sub1Http());
        for (int sequence = 2; sequence <= 4; sequence++) {
            context.publisher().publish(
                "all",
                new Contracts.Event("ps-a4-gap", sequence, "while-sub-1-down-" + sequence));
        }
        ScenarioAssert.waitForSequenceAtLeast(context.evidence(), "sub-2", "ps-a4-gap", 4);

        try (var restarted = context.processes().startSubscriber(
            "sub-1",
            "alpha",
            context.options().sub1Http())) {
            for (int sequence = 5; sequence <= 8; sequence++) {
                context.publisher().publish(
                    "all",
                    new Contracts.Event("ps-a4-after", sequence, "after-sub-1-restart-" + sequence));
            }
            ScenarioAssert.waitForSequenceAtLeast(context.evidence(), "sub-1", "ps-a4-after", 8);
            ScenarioAssert.waitForSequenceAtLeast(context.evidence(), "sub-2", "ps-a4-after", 8);
            Contracts.EvidenceSnapshot restartedEvidence = context.evidence().snapshot("sub-1");
            for (int sequence = 2; sequence <= 4; sequence++) {
                ScenarioAssert.ensure(
                    !ScenarioAssert.hasEvent(restartedEvidence, "ps-a4-gap", sequence),
                    "PS-A4 restarted subscriber received event from disconnected interval");
            }
        }
        System.out.println("scenario PS-A4 passed");
    }
}
