package Scenarios;

import systems.zlink.e2e.spotservice.client.Scenarios;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA11Scenario extends SpotServiceScenarioContext {
    private SmA11Scenario(SpotServiceScenarioContext context) { super(context); }

    public static void run(SpotServiceScenarioContext context) { new SmA11Scenario(context).execute(); }

    private void execute() {
        Contracts.ReservedEntryProbeRes result = postJson(options().httpAEndpoint(),
            "/spot/reserved-entry-probe", new Contracts.OperationAccepted(false),
            Contracts.ReservedEntryProbeRes.class);
        ensure(result.entrySpotId() != null && !result.entrySpotId().isBlank()
                && !"none".equals(result.userErrorKind()) && result.userFactoryCalls() == 0,
            "SM-A11 reserved Entry Spot ID was accepted by the User Spot factory");
        System.out.println("scenario SM-A11 passed");
    }
}
