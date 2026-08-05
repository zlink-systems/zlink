package systems.zlink.e2e.spotservice.client.Scenarios;

import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmC3Scenario extends SpotServiceScenarioContext {
    private SmC3Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmC3Scenario(context).execute();
    }

    private void execute() {
        Contracts.StateRes requestReply = eventually(() -> requestState("room-a", "c3-source", REQUEST_TIMEOUT));
        ensure("play-a".equals(requestReply.nodeRid()), "SM-C3 source spot owner mismatch");
        sendOutbound("room-b", "c3-send");
        Contracts.OutboundRes reply = eventually(() -> requestOutbound("room-b", "c3-request"));
        ensure("room-b".equals(reply.spotRid()), "SM-C3 wrong target spot");
        ensure("play-b".equals(reply.nodeRid()), "SM-C3 wrong target node");
        System.out.println("scenario SM-C3 passed");

    }
}
