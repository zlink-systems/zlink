package systems.zlink.e2e.registrymessaging.client.Scenarios;

import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmC2TargetedRouteScenario {
    private RmC2TargetedRouteScenario() {
    }

    public static void run(ZLinkHttpClient providerA) {
        Contracts.RouteRes toB = providerA.post("/profile/route/request")
            .body(new Contracts.RouteReq("target-b"))
            .submit(Contracts.RouteRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that("api-b".equals(toB.targetRid()), "RM-C2 target rid mismatch");

        Contracts.RequestFailureRes missing = providerA.post("/profile/route/missing")
            .body(new Contracts.RouteReq("missing"))
            .submit(Contracts.RequestFailureRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that(missing.failed(), "RM-C2 missing rid request should fail");
        ScenarioAssert.that("REQUEST_TARGET_NOT_FOUND".equals(missing.errorKind()),
            "RM-C2 expected public target-not-found error, got " + missing.errorKind());
        System.out.println("scenario RM-C2 passed");
    }
}
