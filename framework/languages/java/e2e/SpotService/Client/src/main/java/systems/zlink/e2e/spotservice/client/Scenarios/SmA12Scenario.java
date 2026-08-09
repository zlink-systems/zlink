package Scenarios;

import systems.zlink.e2e.spotservice.client.Scenarios;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA12Scenario extends SpotServiceScenarioContext {
    private SmA12Scenario(SpotServiceScenarioContext context) { super(context); }

    public static void run(SpotServiceScenarioContext context) {
        new SmA12Scenario(context).execute();
    }

    private void execute() {
        Contracts.AutomaticSpotBatchRes result = postJson(options().httpAEndpoint(),
            "/spot/create-automatic-batch", new Contracts.AutomaticSpotBatchReq(200),
            Contracts.AutomaticSpotBatchRes.class, EVENTUAL_TIMEOUT);
        ensure(result.requested() == 200 && result.created() == 200
                && result.distinctIds() == 200 && result.successfulRequests() == 200,
            "SM-A12 automatic Spot creation did not preserve unique routable actors");
        System.out.println("scenario SM-A12 passed");
    }
}
