package systems.zlink.e2e.spotservice.client.Scenarios;

import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA3Scenario extends SpotServiceScenarioContext {
    private SmA3Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmA3Scenario(context).execute();
    }

    private void execute() {
        Contracts.StateRes roomA = eventually(() -> requestState("room-a", "owner-a", REQUEST_TIMEOUT));
        Contracts.StateRes roomB = eventually(() -> requestState("room-b", "owner-b", REQUEST_TIMEOUT));
        ensure("play-a".equals(roomA.nodeRid()), "SM-A3 room-a owner mismatch");
        ensure("play-b".equals(roomB.nodeRid()), "SM-A3 room-b owner mismatch");
        System.out.println("scenario SM-A3 passed");

    }
}
