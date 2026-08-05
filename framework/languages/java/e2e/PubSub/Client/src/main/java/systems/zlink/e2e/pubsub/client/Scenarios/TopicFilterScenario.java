package systems.zlink.e2e.pubsub.client.Scenarios;

import systems.zlink.e2e.pubsub.client.Support.ScenarioAssert;
import systems.zlink.e2e.pubsub.client.Support.ScenarioContext;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class TopicFilterScenario {
    private TopicFilterScenario() {
    }

    public static void run(ScenarioContext context) {
        context.publisher().publish("alpha", new Contracts.EventMsg("ps-a2", 1, "alpha-only"));
        context.publisher().publish("beta", new Contracts.EventMsg("ps-a2", 2, "beta-only"));
        context.publisher().publish("gamma", new Contracts.EventMsg("ps-a2", 3, "gamma-only"));

        ScenarioAssert.waitForEvent(context.evidence(), "sub-1", "ps-a2", 1);
        ScenarioAssert.waitForEvent(context.evidence(), "sub-2", "ps-a2", 2);
        ScenarioAssert.waitForEvent(context.evidence(), "sub-3", "ps-a2", 3);
        ScenarioAssert.sleep(500);

        Contracts.EvidenceSnapshot sub1 = context.evidence().snapshot("sub-1");
        Contracts.EvidenceSnapshot sub2 = context.evidence().snapshot("sub-2");
        Contracts.EvidenceSnapshot sub3 = context.evidence().snapshot("sub-3");
        ScenarioAssert.ensure(ScenarioAssert.hasEvent(sub1, "ps-a2", 1), "PS-A2 sub-1 missed alpha");
        ScenarioAssert.ensure(!ScenarioAssert.hasEvent(sub1, "ps-a2", 2)
                && !ScenarioAssert.hasEvent(sub1, "ps-a2", 3),
            "PS-A2 sub-1 recorded an uninterested topic");
        ScenarioAssert.ensure(ScenarioAssert.hasEvent(sub2, "ps-a2", 2), "PS-A2 sub-2 missed beta");
        ScenarioAssert.ensure(!ScenarioAssert.hasEvent(sub2, "ps-a2", 1)
                && !ScenarioAssert.hasEvent(sub2, "ps-a2", 3),
            "PS-A2 sub-2 recorded an uninterested topic");
        ScenarioAssert.ensure(ScenarioAssert.hasEvent(sub3, "ps-a2", 3), "PS-A2 sub-3 missed gamma");
        ScenarioAssert.ensure(!ScenarioAssert.hasEvent(sub3, "ps-a2", 1)
                && !ScenarioAssert.hasEvent(sub3, "ps-a2", 2),
            "PS-A2 sub-3 recorded an uninterested topic");
        System.out.println("scenario PS-A2 passed");
    }
}
