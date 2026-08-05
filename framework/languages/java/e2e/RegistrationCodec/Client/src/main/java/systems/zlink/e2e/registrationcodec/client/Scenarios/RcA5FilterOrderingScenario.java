package systems.zlink.e2e.registrationcodec.client.Scenarios;

import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class RcA5FilterOrderingScenario {
    private RcA5FilterOrderingScenario() {
    }

    public static void run(ScenarioContext context) {
        Contracts.EchoRes filtered = context.server().post("/registration/filter-order").submit(Contracts.EchoRes.class).toCompletableFuture().join().body();
        ScenarioAssert.ensure("echo:filter-order-request".equals(filtered.value()), "RC-A5 request mismatch");
        ScenarioAssert.waitForFilterOrder(context.evidence(), "filter-order-request");
        System.out.println("scenario RC-A5 passed");
    }
}
