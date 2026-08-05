package systems.zlink.e2e.registrymessaging.client.Scenarios;

import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmA1DiscoveryRequestScenario {
    private RmA1DiscoveryRequestScenario() {
    }

    public static void run(
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB,
        ZLinkHttpClient discoveryConsumer) {
        Contracts.ProfileRes reply = discoveryConsumer.post("/profile/request")
            .body(new Contracts.ProfileReq("rm-a1"))
            .submit(Contracts.ProfileRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that("profile:rm-a1".equals(reply.value()), "RM-A1 reply payload mismatch");
        ScenarioAssert.that(reply.providerRid().equals("api-a") || reply.providerRid().equals("api-b"),
            "RM-A1 provider rid mismatch");

        java.util.Map[] peers = discoveryConsumer.get("/locations/peers")
            .submit(java.util.Map[].class).toCompletableFuture().join().body();
        long readyProviders = java.util.Arrays.stream(peers)
            .filter(entry -> Contracts.API_CHANNEL.equals(entry.get("meshName")))
            .filter(entry -> "ROUTER".equals(entry.get("role")))
            .count();
        ScenarioAssert.that(readyProviders >= 2, "RM-A1 expected at least two live provider peer rows");
        String[] evidence = ScenarioAssert.concat(
            ScenarioAssert.evidence(providerA),
            ScenarioAssert.evidence(providerB));
        ScenarioAssert.that(java.util.Arrays.stream(evidence)
                .anyMatch(line -> line.contains("ProfileReq") && line.contains("rm-a1")),
            "RM-A1 provider evidence missing");
        System.out.println("scenario RM-A1 passed");
    }
}
