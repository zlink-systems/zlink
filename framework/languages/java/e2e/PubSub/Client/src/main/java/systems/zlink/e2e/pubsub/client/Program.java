package systems.zlink.e2e.pubsub.client;

import systems.zlink.e2e.pubsub.client.Scenarios.FanoutBasicDeliveryScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.LateSubscriberScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.MissingMessageNameScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.PublisherRestartScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.SlowSubscriberScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.SubscriberReconnectScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.TopicFilterScenario;
import systems.zlink.e2e.pubsub.client.Support.ScenarioAssert;
import systems.zlink.e2e.pubsub.client.Support.ScenarioContext;
import systems.zlink.e2e.pubsub.shared.Contracts;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        try (ScenarioContext context = ScenarioContext.load(args)) {
            switch (context.options().mode()) {
                case "PS-A1" -> FanoutBasicDeliveryScenario.run(context);
                case "PS-A2" -> TopicFilterScenario.run(context);
                case "PS-A3" -> runLateSubscriberOnly(context);
                case "PS-A4", "subscriber-restarted" -> SubscriberReconnectScenario.run(context);
                case "PS-B1", "slow-subscriber" -> SlowSubscriberScenario.run(context);
                case "PS-B2", "publisher-restarted" -> PublisherRestartScenario.run(context);
                case "PS-C1" -> MissingMessageNameScenario.run(context);
                case "default" -> runDefault(context);
                default -> throw new IllegalArgumentException(
                    "unknown mode " + context.options().mode());
            }
            System.out.println("pub-sub e2e result=passed");
        }
    }

    private static void runDefault(ScenarioContext context) {
        ScenarioAssert.touch(context.options().publisherReadyFile());
        ScenarioAssert.waitForFile(context.options().prelateContinueFile());

        context.publisher().publish("all", new Contracts.EventMsg("prelate", 0, "before-late"));
        ScenarioAssert.waitForEvent(context.evidence(), "sub-1", "prelate", 0);
        ScenarioAssert.waitForEvent(context.evidence(), "sub-2", "prelate", 0);
        ScenarioAssert.touch(context.options().lateReadyFile());
        ScenarioAssert.waitForFile(context.options().lateContinueFile());

        FanoutBasicDeliveryScenario.run(context);
        TopicFilterScenario.run(context);
        LateSubscriberScenario.run(context);
        MissingMessageNameScenario.run(context);
    }

    private static void runLateSubscriberOnly(ScenarioContext context) {
        ScenarioAssert.touch(context.options().publisherReadyFile());
        ScenarioAssert.waitForFile(context.options().prelateContinueFile());

        context.publisher().publish("all", new Contracts.EventMsg("prelate", 0, "before-late"));
        ScenarioAssert.waitForEvent(context.evidence(), "sub-1", "prelate", 0);
        ScenarioAssert.waitForEvent(context.evidence(), "sub-2", "prelate", 0);
        ScenarioAssert.touch(context.options().lateReadyFile());
        ScenarioAssert.waitForFile(context.options().lateContinueFile());

        LateSubscriberScenario.run(context);
    }
}
