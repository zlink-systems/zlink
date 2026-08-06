package systems.zlink.e2e.pubsub.client;

import systems.zlink.e2e.pubsub.client.Scenarios.FanoutBasicDeliveryScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.LateSubscriberScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.MissingMessageNameScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.PublisherRestartScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.SlowSubscriberScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.SubscriberReconnectScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.TopicFilterScenario;
import systems.zlink.e2e.pubsub.client.Scenarios.CommonPubSubScenarios;
import systems.zlink.e2e.pubsub.client.Scenarios.StartupValidationScenarios;
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
                case "PS-D1" -> CommonPubSubScenarios.runD1(context);
                case "PS-D2" -> CommonPubSubScenarios.runD2(context);
                case "PS-D3" -> CommonPubSubScenarios.runD3(context);
                case "PS-D4" -> CommonPubSubScenarios.runD4(context);
                case "PS-D5" -> CommonPubSubScenarios.runD5(context, false);
                case "PS-D5-RECOVERY" -> CommonPubSubScenarios.runD5(context, true);
                case "PS-D6" -> CommonPubSubScenarios.runD6(context);
                case "PS-D7A" -> CommonPubSubScenarios.runD7A(context);
                case "PS-D7B" -> CommonPubSubScenarios.runD7B(context);
                case "PS-E1" -> CommonPubSubScenarios.runE1(context);
                case "PS-E2A" -> StartupValidationScenarios.runE2A(context);
                case "PS-E2B" -> StartupValidationScenarios.runE2B(context);
                case "PS-E2C" -> StartupValidationScenarios.runE2C(context);
                case "PS-F1" -> CommonPubSubScenarios.runF1(context);
                case "PS-F2" -> CommonPubSubScenarios.runF2(context);
                case "PS-F3" -> CommonPubSubScenarios.runF3(context);
                case "PS-F4" -> CommonPubSubScenarios.runF4(context);
                case "PS-F5" -> CommonPubSubScenarios.runF5(context);
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
