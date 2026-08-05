package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.UUID;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdC1TimerIsolationScenario {
    private AtdC1TimerIsolationScenario() {
    }

    public static void run(ZLinkStreamConnector connector) {
        String spotRid = "await-timer-" + UUID.randomUUID().toString().replace("-", "");
        Contracts.EnsureSpotRes spot = ClientStreamSupport.await(
            connector.request(new Contracts.EnsureSpotReq(spotRid))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.EnsureSpotRes.class);
        ScenarioAssert.that(spotRid.equals(spot.spotRid()), "ATD-C1 timer spot creation mismatch");
        String requestId = "ATD-C1-" + UUID.randomUUID().toString().replace("-", "");
        String awaitTimer = requestId + "-await";
        String fastTimer = requestId + "-fast";
        ClientStreamSupport.send(
            connector.send(new Contracts.TimerStartMsg(
                    requestId,
                    awaitTimer,
                    "await-on-first",
                    50,
                    350))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, spotRid));
        ClientStreamSupport.waitForEvidence(connector, requestId, spotRid, "timer-await-released");
        ClientStreamSupport.send(
            connector.send(new Contracts.TimerStartMsg(
                    requestId,
                    fastTimer,
                    "fast",
                    50,
                    0))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, spotRid));
        ClientStreamSupport.waitForEvidence(connector, requestId, spotRid, "timer-fast-completed");
        Contracts.EvidenceRes evidence = ClientStreamSupport.waitForEvidence(
            connector,
            requestId,
            spotRid,
            "timer-await-completed");
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "timer-await-started",
            "timer-await-released",
            "timer-fast-started",
            "timer-fast-completed",
            "timer-await-resumed",
            "timer-await-completed");
        ClientStreamSupport.send(
            connector.send(new Contracts.TimerStopMsg(requestId))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, spotRid));
        System.out.println("scenario ATD-C1 passed");
    }
}
