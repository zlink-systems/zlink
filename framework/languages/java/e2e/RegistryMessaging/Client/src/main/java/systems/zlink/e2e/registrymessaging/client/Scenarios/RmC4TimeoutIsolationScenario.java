package systems.zlink.e2e.registrymessaging.client.Scenarios;

import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.client.Support.ScenarioWait;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmC4TimeoutIsolationScenario {
    private RmC4TimeoutIsolationScenario() {
    }

    public static void run(
        ZLinkHttpClient discoveryConsumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB) {
        Contracts.RequestFailureRes timeout = discoveryConsumer.post("/profile/slow-request")
            .body(new Contracts.ProfileReq("slow"))
            .submit(Contracts.RequestFailureRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that(timeout.failed(), "RM-C4 expected slow request failure");
        ScenarioAssert.that("TimeoutException".equals(timeout.errorKind()),
            "RM-C4 expected public TimeoutException, got " + timeout.errorKind());
        Contracts.ProfileRes after = discoveryConsumer.post("/profile/request")
            .body(new Contracts.ProfileReq("after"))
            .submit(Contracts.ProfileRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that("profile:after".equals(after.value()), "RM-C4 post-timeout request failed");
        ScenarioWait.sleep(1100);
        Contracts.ProfileRes later = discoveryConsumer.post("/profile/request")
            .body(new Contracts.ProfileReq("later"))
            .submit(Contracts.ProfileRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that("profile:later".equals(later.value()), "RM-C4 later request failed");
        String[] slowEvidence = ScenarioAssert.waitAnyEvidence(providerA, providerB, "value=slow");
        ScenarioAssert.that(java.util.Arrays.stream(slowEvidence)
                .anyMatch(line -> line.contains("ProfileReq") && line.contains("value=slow")),
            "RM-C4 slow handler completion evidence missing");
        System.out.println("scenario RM-C4 passed");
    }
}
