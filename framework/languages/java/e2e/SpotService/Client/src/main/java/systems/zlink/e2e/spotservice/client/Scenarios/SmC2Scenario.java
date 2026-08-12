package systems.zlink.e2e.spotservice.client.Scenarios;

import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmC2Scenario extends SpotServiceScenarioContext {
    private SmC2Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmC2Scenario(context).execute();
    }

    private void execute() {
        Contracts.OutboundRes reply = eventually(() -> requestOutbound("room-a", "c2"));
        ensure("room-a".equals(reply.spotRid()), "SM-C2 wrong source spot");
        ensure("play-a".equals(reply.nodeRid()), "SM-C2 wrong source node");
        ensure("c2".equals(reply.channelReply()), "SM-C2 channel request reply mismatch");
        System.out.println("scenario SM-C2 passed");

    }
}
