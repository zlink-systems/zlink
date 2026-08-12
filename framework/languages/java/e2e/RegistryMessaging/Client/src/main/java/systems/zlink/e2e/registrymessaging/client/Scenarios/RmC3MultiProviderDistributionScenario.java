package systems.zlink.e2e.registrymessaging.client.Scenarios;

import java.util.HashMap;
import java.util.Map;
import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmC3MultiProviderDistributionScenario {
    private RmC3MultiProviderDistributionScenario() {
    }

    public static void run(
        ZLinkHttpClient directConsumer,
        String scenarioId,
        String prefix,
        int requestCount,
        boolean requireHighGreaterThanLow) {
        Map<String, Integer> counts = new HashMap<>();
        Contracts.ProfileReq[] requests = new Contracts.ProfileReq[requestCount];
        for (int index = 0; index < requestCount; index++) {
            requests[index] = new Contracts.ProfileReq(prefix + index);
        }
        Contracts.ProfileRes[] replies = directConsumer.post("/profile/batch-request")
            .body(requests)
            .submit(Contracts.ProfileRes[].class).toCompletableFuture().join().body();
        for (Contracts.ProfileRes reply : replies) {
            counts.merge(reply.providerRid(), 1, Integer::sum);
        }
        ScenarioAssert.that(counts.getOrDefault("api-a", 0) > 0, scenarioId + " did not reach api-a");
        ScenarioAssert.that(counts.getOrDefault("api-b", 0) > 0, scenarioId + " did not reach api-b");
        ScenarioAssert.that(counts.values().stream().mapToInt(Integer::intValue).sum() == requestCount,
            scenarioId + " count mismatch");
        if (requireHighGreaterThanLow) {
            ScenarioAssert.that(counts.getOrDefault("api-a", 0) > counts.getOrDefault("api-b", 0),
                scenarioId + " high weight provider was not preferred: " + counts);
        }
        System.out.println("scenario " + scenarioId + " passed");
    }
}
