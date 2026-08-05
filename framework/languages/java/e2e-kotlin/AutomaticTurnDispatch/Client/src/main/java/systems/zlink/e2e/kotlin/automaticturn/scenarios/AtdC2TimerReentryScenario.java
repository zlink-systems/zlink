package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.UUID;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdC2TimerReentryScenario {
    private AtdC2TimerReentryScenario() {
    }

    public static void run(ZLinkStreamConnector connector) {
        String spotRid = "await-timer-" + UUID.randomUUID().toString().replace("-", "");
        Contracts.EnsureSpotRes spot = ClientStreamSupport.await(
            connector.request(new Contracts.EnsureSpotReq(spotRid))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.EnsureSpotRes.class);
        ScenarioAssert.that(spotRid.equals(spot.spotRid()), "ATD-C2 timer spot creation mismatch");
        String requestId = "ATD-C2-" + UUID.randomUUID().toString().replace("-", "");
        String timerName = requestId + "-same";
        ClientStreamSupport.send(
            connector.send(new Contracts.TimerStartMsg(
                    requestId,
                    timerName,
                    "await-then-next",
                    50,
                    350))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, spotRid));
        Contracts.EvidenceRes evidence = ClientStreamSupport.waitForEvidence(
            connector,
            requestId,
            spotRid,
            "timer-next-completed");
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "timer-await-started",
            "timer-await-released",
            "timer-await-resumed",
            "timer-await-completed",
            "timer-next-started",
            "timer-next-completed");
        ClientStreamSupport.send(
            connector.send(new Contracts.TimerStopMsg(requestId))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, spotRid));
        System.out.println("scenario ATD-C2 passed");
    }
}
