package Scenarios;

import systems.zlink.e2e.pubsub.client.Scenarios;
import systems.zlink.e2e.pubsub.client.Support;
import java.util.List;
import java.util.Set;
import systems.zlink.e2e.pubsub.client.Support.ScenarioAssert;
import systems.zlink.e2e.pubsub.client.Support.ScenarioContext;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class FanoutBasicDeliveryScenario {
    private FanoutBasicDeliveryScenario() {
    }

    public static void run(ScenarioContext context) {
        for (int index = 0; index < 20; index++) {
            context.publisher().publish("all", new Contracts.EventMsg("warmup", index, "warmup-" + index));
        }
        for (String rid : List.of("sub-1", "sub-2", "sub-3")) {
            ScenarioAssert.waitForAnyEvent(context.evidence(), rid, "warmup");
        }

        for (int sequence = 0; sequence < 12; sequence++) {
            context.publisher().publish("all", new Contracts.EventMsg("ps-a1", sequence, "fanout-" + sequence));
        }
        List<Integer> common = ScenarioAssert.commonSequences(
            context.evidence(),
            "ps-a1",
            Set.of("sub-1", "sub-2", "sub-3"));
        ScenarioAssert.ensure(
            ScenarioAssert.hasContiguousRun(common, 4),
            "PS-A1 did not observe a shared contiguous sequence");
        System.out.println("scenario PS-A1 passed");
    }
}
