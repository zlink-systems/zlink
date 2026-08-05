package systems.zlink.e2e.registrymessaging.client.Scenarios;

import java.util.Set;
import systems.zlink.e2e.registrymessaging.client.Support.DynamicClusterLauncher;
import systems.zlink.e2e.registrymessaging.client.Support.ClientOptions;
import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmB1ScaleOutScenario {
    private RmB1ScaleOutScenario() {
    }

    public static void run(ClientOptions options) {
        try (DynamicClusterLauncher cluster = DynamicClusterLauncher.start(options)) {
            DynamicClusterLauncher.DynamicProvider providerA = cluster.startProvider("api-a", "api-a");
            DynamicClusterLauncher.DynamicConsumer consumer = cluster.startConsumer("consumer");
            try (ZLinkHttpClient requester = ZLinkHttpClient.create(consumer.httpUrl()).build()) {
                cluster.waitPeerEndpoint(requester, providerA.routingId());
                for (int index = 0; index < 5; index++) {
                    Contracts.ProfileRes reply =
                        ScenarioAssert.requestProfileEventually(requester, "scale-out-before-" + index);
                    ScenarioAssert.that("api-a".equals(reply.providerRid()),
                        "RM-B1 initial traffic should only use api-a");
                }

                DynamicClusterLauncher.DynamicProvider providerB = cluster.startProvider("api-b", "api-b");
                cluster.waitPeerEndpoint(requester, providerB.routingId());
                cluster.waitPeerCount(requester, 2);

                Set<String> providers = ScenarioAssert.requestUntilProvidersSeen(
                    requester,
                    "scale-out-after",
                    Set.of("api-a", "api-b"));
                ScenarioAssert.that(providers.contains("api-a") && providers.contains("api-b"),
                    "RM-B1 did not route to both providers after scale-out");
            }
        }
        System.out.println("scenario RM-B1 passed");
    }
}
