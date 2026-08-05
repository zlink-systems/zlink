package systems.zlink.e2e.registrymessaging.client.Support;

import systems.zlink.e2e.registrymessaging.client.Scenarios.RmA1DiscoveryRequestScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmA2ManualEndpointScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmA4SameRidReplacementScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmA6MultipleChannelsScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmB1ScaleOutScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmB2ScaleInScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmC1RequestSendScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmC2TargetedRouteScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmC3MultiProviderDistributionScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmC4TimeoutIsolationScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmC5MissingPacketScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmC7WeightedProviderScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmC8PayloadRoundTripScenario;
import systems.zlink.e2e.registrymessaging.client.Scenarios.RmC9BackpressureScenario;

public final class ScenarioCatalog {
    private final RegistryMessagingHttp http;
    private final ClientOptions options;

    public ScenarioCatalog(RegistryMessagingHttp http, ClientOptions options) {
        this.http = http;
        this.options = options;
    }

    public void run(String scenario) {
        switch (scenario) {
            case "common" -> runCommon();
            case "RM-A1" -> RmA1DiscoveryRequestScenario.run(
                http.providerA(),
                http.providerB(),
                http.discoveryConsumer());
            case "RM-A2" -> RmA2ManualEndpointScenario.run(http.singleConsumer(), http.providerA());
            case "RM-A6" -> RmA6MultipleChannelsScenario.run(
                http.discoveryConsumer(),
                http.providerA(),
                http.providerB(),
                http.workflow());
            case "scale-out" -> RmB1ScaleOutScenario.run(options);
            case "scale-in" -> RmB2ScaleInScenario.run(options);
            case "RM-B1" -> RmB1ScaleOutScenario.run(options);
            case "RM-B2" -> RmB2ScaleInScenario.run(options);
            case "RM-A4" -> RmA4SameRidReplacementScenario.run(options);
            case "RM-C1" -> RmC1RequestSendScenario.run(http.providerA(), http.providerB());
            case "RM-C2" -> RmC2TargetedRouteScenario.run(http.providerA());
            case "RM-C3" -> RmC3MultiProviderDistributionScenario.run(http.directConsumer(), "RM-C3", "multi-", 80, false);
            case "RM-C4" -> RmC4TimeoutIsolationScenario.run(
                http.discoveryConsumer(),
                http.providerA(),
                http.providerB());
            case "RM-C5" -> RmC5MissingPacketScenario.run(http.discoveryConsumer(), http.providerA(), http.providerB());
            case "weighted" -> RmC7WeightedProviderScenario.run(http.directConsumer());
            case "RM-C7" -> RmC7WeightedProviderScenario.run(http.directConsumer());
            case "RM-C8" -> RmC8PayloadRoundTripScenario.run(
                http.singleConsumer(),
                http.providerA(),
                http.providerB());
            case "RM-C9" -> RmC9BackpressureScenario.run(
                http.backpressureConsumer(),
                http.providerA(),
                http.providerB());
            default -> throw new IllegalArgumentException(
                "unknown scenario " + scenario);
        }
    }

    private void runCommon() {
        RmA1DiscoveryRequestScenario.run(http.providerA(), http.providerB(), http.discoveryConsumer());
        RmC1RequestSendScenario.run(http.providerA(), http.providerB());
        RmC4TimeoutIsolationScenario.run(
            http.discoveryConsumer(),
            http.providerA(),
            http.providerB());
        RmC5MissingPacketScenario.run(http.discoveryConsumer(), http.providerA(), http.providerB());
        RmA2ManualEndpointScenario.run(http.singleConsumer(), http.providerA());
        RmC3MultiProviderDistributionScenario.run(http.directConsumer(), "RM-C3", "multi-", 80, false);
        RmA6MultipleChannelsScenario.run(
            http.discoveryConsumer(),
            http.providerA(),
            http.providerB(),
            http.workflow());
        RmC8PayloadRoundTripScenario.run(http.singleConsumer(), http.providerA(), http.providerB());
        RmC9BackpressureScenario.run(
            http.backpressureConsumer(),
            http.providerA(),
            http.providerB());
        RmC2TargetedRouteScenario.run(http.providerA());
    }
}
