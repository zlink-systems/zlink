package systems.zlink.e2e.spotservice.client.Scenarios;

import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA4Scenario {
    private SmA4Scenario() {
    }

    public static void run(SpotServiceScenarioContext context) {
        String sourceEndpoint = context.options().httpAEndpoint();
        try {
            Contracts.StateRes before = context.eventually(
                () -> context.requestState("room-a", "a4-before", context.REQUEST_TIMEOUT));
            context.ensure("play-a".equals(before.nodeRid()),
                "SM-A4 initial owner mismatch");

            context.setPlacementWeight(context.options().httpAEndpoint(), 0);
            context.setPlacementWeight(context.options().httpBEndpoint(), 100);
            Contracts.RelocationRes relocation = context.relocate(sourceEndpoint);
            context.ensure("RELOCATED".equals(relocation.outcome()),
                "SM-A4 relocation did not complete: " + relocation.outcome()
                    + "/" + relocation.reason());

            Contracts.StateRes after = context.eventually(
                () -> context.requestState("room-a", "a4-after", context.REQUEST_TIMEOUT));
            context.ensure("play-b".equals(after.nodeRid()),
                "SM-A4 relocated owner mismatch");
            System.out.println("scenario SM-A4 passed");
        } finally {
            context.setPlacementWeight(context.options().httpAEndpoint(), 100);
            context.setPlacementWeight(context.options().httpBEndpoint(), 100);
        }
    }
}
