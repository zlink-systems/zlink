package systems.zlink.e2e.pubsub.client.Scenarios;

import java.util.Set;
import systems.zlink.e2e.pubsub.client.Support.ScenarioAssert;
import systems.zlink.e2e.pubsub.client.Support.ScenarioContext;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class PublisherRestartScenario {
    private PublisherRestartScenario() {
    }

    public static void run(ScenarioContext context) {
        context.processes().waitPublisherRow(false);
        var publisher = context.processes().startPublisher("publisher-baseline");
        try {
            context.processes().waitPublisherRow(true);
            for (int sequence = 1; sequence <= 80; sequence++) {
                context.publisher().publish(
                    "all",
                    new Contracts.EventMsg(
                        "ps-b2",
                        sequence,
                        "before-publisher-restart-" + sequence));
                ScenarioAssert.sleep(100);
            }
            var common = ScenarioAssert.commonSequences(
                context.evidence(),
                "ps-b2",
                Set.of("sub-1", "sub-2", "sub-3"));
            if (common.isEmpty()) {
                throw new IllegalStateException("PS-B2 subscribers did not receive baseline publisher events");
            }
            String drainResult = context.processes().drainPublisher(publisher);
            ScenarioAssert.ensure("Drained".equals(drainResult),
                "PS-B2 publisher did not reach terminal Drained: " + drainResult);
            context.processes().waitPublisherRow(false);
        } finally {
            publisher.close();
        }

        context.processes().waitStopped("publisher", context.options().publisherHttp());
        ScenarioAssert.expectPublishFailure(
            () -> context.publisher().publish("all", new Contracts.EventMsg("ps-b2", 2, "during-publisher-down")),
            "PS-B2 expected publish to fail while publisher process is down");

        try (var restarted = context.processes().startPublisher("publisher-restarted")) {
            context.processes().waitPublisherRow(true);
            for (int sequence = 3; sequence <= 42; sequence++) {
                context.publisher().publish(
                    "all",
                    new Contracts.EventMsg(
                        "ps-b2",
                        sequence,
                        "after-publisher-restart-" + sequence));
                ScenarioAssert.sleep(100);
            }
            ScenarioAssert.waitForSequenceAtLeast(context.evidence(), "sub-1", "ps-b2", 20);
            ScenarioAssert.waitForSequenceAtLeast(context.evidence(), "sub-2", "ps-b2", 20);
            ScenarioAssert.waitForSequenceAtLeast(context.evidence(), "sub-3", "ps-b2", 20);
            System.out.println("scenario PS-B2 passed");
            return;
        }
    }
}
