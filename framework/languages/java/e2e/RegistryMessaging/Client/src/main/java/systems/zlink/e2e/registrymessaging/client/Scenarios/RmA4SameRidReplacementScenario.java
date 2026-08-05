package systems.zlink.e2e.registrymessaging.client.Scenarios;

import systems.zlink.e2e.registrymessaging.client.Support.DynamicClusterLauncher;
import systems.zlink.e2e.registrymessaging.client.Support.ClientOptions;
import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmA4SameRidReplacementScenario {
    private RmA4SameRidReplacementScenario() {
    }

    public static void run(ClientOptions options) {
        try (DynamicClusterLauncher cluster = DynamicClusterLauncher.start(options)) {
            DynamicClusterLauncher.DynamicProvider providerV1 =
                cluster.startProvider("api-a-v1", "api-a", "api-a-v1", "");
            DynamicClusterLauncher.DynamicConsumer consumer = cluster.startConsumer("consumer");
            try (ZLinkHttpClient requester = ZLinkHttpClient.create(consumer.httpUrl()).build()) {
                cluster.waitPeerEndpoint(requester, providerV1.routingId());
                Contracts.ProfileRes first = ScenarioAssert.requestProfileEventually(requester, "replacement-before");
                ScenarioAssert.that("api-a".equals(first.providerRid()) && "api-a-v1".equals(first.instanceId()),
                    "RM-A4 initial provider mismatch");
            }

            String drainResult = cluster.stop(providerV1);
            ScenarioAssert.that("Drained".equals(drainResult),
                "RM-A4 v1 did not reach terminal Drained: " + drainResult);
            try (ZLinkHttpClient requester = ZLinkHttpClient.create(consumer.httpUrl()).build()) {
                cluster.waitPeerEndpointAbsent(requester, providerV1.routingId());
            }
            DynamicClusterLauncher.DynamicProvider providerV2 =
                cluster.startProvider("api-a-v2", "api-a", "api-a-v2", "");
            try (ZLinkHttpClient requester = ZLinkHttpClient.create(consumer.httpUrl()).build()) {
                cluster.waitSinglePeer(requester, "api-a", providerV2.routingId());
                for (int index = 0; index < 20; index++) {
                    Contracts.ProfileRes reply =
                        ScenarioAssert.requestProfileEventually(requester, "replacement-after-" + index);
                    ScenarioAssert.that("api-a".equals(reply.providerRid()) && "api-a-v2".equals(reply.instanceId()),
                        "RM-A4 did not switch to replacement provider");
                }
            }
        }
        System.out.println("scenario RM-A4 passed");
    }
}
