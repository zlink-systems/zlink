package systems.zlink.e2e.spotservice.client.Scenarios;

import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA1Scenario extends SpotServiceScenarioContext {
    private SmA1Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmA1Scenario(context).execute();
    }

    private void execute() {
        Contracts.StateRes first = eventually(() -> requestState("room-a", "a1", REQUEST_TIMEOUT));
        ensure("room-a".equals(first.spotRid()), "SM-A1 wrong spot rid");
        ensure("play-a".equals(first.nodeRid()), "SM-A1 wrong owner node");
        System.out.println("scenario SM-A1 passed");

    }
}
