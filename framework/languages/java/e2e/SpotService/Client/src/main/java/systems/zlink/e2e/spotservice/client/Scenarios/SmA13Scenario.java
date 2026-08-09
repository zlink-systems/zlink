package Scenarios;

import systems.zlink.e2e.spotservice.client.Scenarios;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA13Scenario extends SpotServiceScenarioContext {
    private SmA13Scenario(SpotServiceScenarioContext context) { super(context); }

    public static void run(SpotServiceScenarioContext context) {
        new SmA13Scenario(context).execute();
    }

    private void execute() {
        Contracts.SpotIdBoundaryRes result = postJson(options().httpAEndpoint(),
            "/spot/id-boundary", null, Contracts.SpotIdBoundaryRes.class, EVENTUAL_TIMEOUT);
        ensure(result.validIds().equals(result.foundIds()) && result.exactEquality(),
            "SM-A13 valid IDs were not preserved exactly");
        ensure(result.stateValues().stream().allMatch(value -> value == 1),
            "SM-A13 valid IDs did not route to their typed handler");
        ensure(!"none".equals(result.invalidErrorKind()) && result.invalidFactoryCalls() == 0,
            "SM-A13 256-character ID was not rejected before factory invocation");
        System.out.println("scenario SM-A13 passed");
    }
}
