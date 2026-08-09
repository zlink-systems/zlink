package Scenarios;

import systems.zlink.e2e.spotservice.client.Scenarios;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA10Scenario extends SpotServiceScenarioContext {
    private SmA10Scenario(SpotServiceScenarioContext context) { super(context); }

    public static void run(SpotServiceScenarioContext context) {
        new SmA10Scenario(context).execute();
    }

    private void execute() {
        Contracts.EntryIdentity first = postJson(options().httpAEndpoint(), "/entry/identity",
            new Contracts.OperationAccepted(false), Contracts.EntryIdentity.class);
        Contracts.EntryIdentity second = postJson(options().httpAEndpoint(), "/entry/identity",
            new Contracts.OperationAccepted(false), Contracts.EntryIdentity.class);
        ensure(first.entrySpotId() != null && !first.entrySpotId().isBlank(),
            "SM-A10 entry Spot ID was not published");
        ensure(first.entrySpotId().equals(second.entrySpotId())
                && first.nodeRid().equals(second.nodeRid()),
            "SM-A10 entry identity changed during one lifecycle");
        ensure(!first.entrySpotId().equals(first.nodeRid()),
            "SM-A10 entry Spot ID was coupled to MeshNode RID");
        System.out.println("scenario SM-A10 passed");
    }
}
