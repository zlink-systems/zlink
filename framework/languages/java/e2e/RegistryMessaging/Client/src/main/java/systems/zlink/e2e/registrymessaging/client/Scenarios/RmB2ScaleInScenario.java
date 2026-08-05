package systems.zlink.e2e.registrymessaging.client.Scenarios;

import java.util.Set;
import systems.zlink.e2e.registrymessaging.client.Support.DynamicClusterLauncher;
import systems.zlink.e2e.registrymessaging.client.Support.ClientOptions;
import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmB2ScaleInScenario {
    private RmB2ScaleInScenario() {
    }

    public static void run(ClientOptions options) {
        try (DynamicClusterLauncher cluster = DynamicClusterLauncher.start(options)) {
            DynamicClusterLauncher.DynamicProvider providerA = cluster.startProvider("api-a", "api-a");
            DynamicClusterLauncher.DynamicProvider providerB = cluster.startProvider("api-b", "api-b");
            DynamicClusterLauncher.DynamicConsumer consumer = cluster.startConsumer("consumer");
            try (ZLinkHttpClient requester = ZLinkHttpClient.create(consumer.httpUrl()).build()) {
                cluster.waitPeerEndpoint(requester, providerA.routingId());
                cluster.waitPeerEndpoint(requester, providerB.routingId());
                cluster.waitPeerCount(requester, 2);
                Set<String> before = ScenarioAssert.requestUntilProvidersSeen(
                    requester,
                    "scale-in-before",
                    Set.of("api-a", "api-b"));
                ScenarioAssert.that(before.contains("api-a") && before.contains("api-b"),
                    "RM-B2 did not start with both providers");

                String drainResult = cluster.stop(providerB);
                ScenarioAssert.that("Drained".equals(drainResult),
                    "RM-B2 provider did not reach terminal Drained: " + drainResult);
                cluster.waitPeerEndpointAbsent(requester, providerB.routingId());

                for (int index = 0; index < 20; index++) {
                    Contracts.ProfileRes reply =
                        ScenarioAssert.requestProfileEventually(requester, "scale-in-after-" + index);
                    ScenarioAssert.that("api-a".equals(reply.providerRid()), "RM-B2 routed to removed provider");
                }
            }
        }
        System.out.println("scenario RM-B2 passed");
    }
}
