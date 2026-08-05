package systems.zlink.e2e.registrymessaging.client.Scenarios;

import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmC5MissingPacketScenario {
    private RmC5MissingPacketScenario() {
    }

    public static void run(
        ZLinkHttpClient discoveryConsumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB) {
        Contracts.RequestFailureRes missing = discoveryConsumer.post("/profile/missing-request")
            .body(new Contracts.ProfileReq("missing"))
            .submit(Contracts.RequestFailureRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that(missing.failed(), "RM-C5 missing request should fail");
        discoveryConsumer.post("/profile/missing-command")
            .body(new Contracts.ProfileMsg("missing-send"))
            .submit(Object.class).toCompletableFuture().join().body();
        String[] requestEvidence = ScenarioAssert.waitAnyEvidence(providerA, providerB, "MissingProfileReq");
        ScenarioAssert.that(java.util.Arrays.stream(requestEvidence)
                .anyMatch(line -> line.contains("dispatch-error")
                    && line.contains("HANDLER_MISSING")
                    && line.contains("REPLY_ERROR")
                    && line.contains("MissingProfileReq")),
            "RM-C5 missing request dispatch-error evidence missing");
        String[] sendEvidence = ScenarioAssert.waitAnyEvidence(providerA, providerB, "MissingProfileMsg");
        ScenarioAssert.that(java.util.Arrays.stream(sendEvidence)
                .anyMatch(line -> line.contains("dispatch-error")
                    && line.contains("HANDLER_MISSING")
                    && line.contains("DROP")
                    && line.contains("MissingProfileMsg")),
            "RM-C5 missing send dispatch-error evidence missing");
        Contracts.ProfileRes normal = discoveryConsumer.post("/profile/request")
            .body(new Contracts.ProfileReq("normal"))
            .submit(Contracts.ProfileRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that("profile:normal".equals(normal.value()), "RM-C5 normal request failed");
        System.out.println("scenario RM-C5 passed");
    }
}
