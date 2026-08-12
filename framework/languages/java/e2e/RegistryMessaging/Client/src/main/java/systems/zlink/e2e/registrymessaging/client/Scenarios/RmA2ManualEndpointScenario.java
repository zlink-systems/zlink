package systems.zlink.e2e.registrymessaging.client.Scenarios;

import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmA2ManualEndpointScenario {
    private RmA2ManualEndpointScenario() {
    }

    public static void run(ZLinkHttpClient singleConsumer, ZLinkHttpClient providerA) {
        Contracts.ProfileRes manual = singleConsumer.post("/profile/request")
            .body(new Contracts.ProfileReq("manual"))
            .submit(Contracts.ProfileRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that("api-a".equals(manual.providerRid()), "RM-A2 wrong provider");
        ScenarioAssert.waitEvidence(providerA, "value=manual");
        System.out.println("scenario RM-A2 passed");
    }
}
