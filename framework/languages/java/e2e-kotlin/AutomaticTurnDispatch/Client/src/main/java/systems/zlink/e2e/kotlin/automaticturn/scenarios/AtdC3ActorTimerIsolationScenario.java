package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdC3ActorTimerIsolationScenario {
    private AtdC3ActorTimerIsolationScenario() {
    }

    public static void run(ZLinkStreamConnector connector) {
        String suffix = UUID.randomUUID().toString().replace("-", "");
        String spotRid = "await-c3-" + suffix;
        String actorA = "actor-c3-a-" + suffix;
        String actorB = "actor-c3-b-" + suffix;
        ClientStreamSupport.bindActors(connector, spotRid, actorA, actorB);
        ClientStreamSupport.joinActor(connector, actorA, spotRid, "c3-a");
        ClientStreamSupport.joinActor(connector, actorB, spotRid, "c3-b");
        verifyActorAwaitAllowsTimer(connector, spotRid, actorA);
        verifyTimerAwaitAllowsActor(connector, spotRid, actorB);
        System.out.println("scenario ATD-C3 passed");
    }

    private static void verifyActorAwaitAllowsTimer(
        ZLinkStreamConnector connector,
        String spotRid,
        String actorId) {
        String requestId = "ATD-C3-actor-" + UUID.randomUUID().toString().replace("-", "");
        CompletionStage<Contracts.ActorRes> await = connector
            .request(new Contracts.ActorAwaitReq(requestId, 1200))
            .metadata("actor-id", actorId)
            .metadata(Contracts.SPOT_RID_METADATA, spotRid)
            .timeout(ClientStreamSupport.REQUEST_TIMEOUT)
            .submit(Contracts.ActorRes.class);
        ClientStreamSupport.waitForEvidence(connector, requestId, spotRid, "actor-await-released");
        ClientStreamSupport.send(
            connector.send(new Contracts.TimerStartMsg(
                    requestId,
                    requestId + "-fast",
                    "fast",
                    50,
                    0))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, spotRid));
        ClientStreamSupport.waitForEvidence(connector, requestId, spotRid, "timer-fast-completed");
        await.toCompletableFuture().join();
        Contracts.EvidenceRes evidence =
            ClientStreamSupport.waitForEvidence(connector, requestId, spotRid, "actor-await-completed");
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "actor-await-started",
            "actor-await-released",
            "timer-fast-started",
            "timer-fast-completed",
            "actor-await-resumed",
            "actor-await-completed");
    }

    private static void verifyTimerAwaitAllowsActor(
        ZLinkStreamConnector connector,
        String spotRid,
        String actorId) {
        String requestId = "ATD-C3-timer-" + UUID.randomUUID().toString().replace("-", "");
        ClientStreamSupport.send(
            connector.send(new Contracts.TimerStartMsg(
                    requestId,
                    requestId + "-await",
                    "await-on-first",
                    50,
                    1200))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .metadata(Contracts.SPOT_RID_METADATA, spotRid));
        ClientStreamSupport.waitForEvidence(connector, requestId, spotRid, "timer-await-released");
        Contracts.ActorRes fast = ClientStreamSupport.await(
            connector.request(new Contracts.ActorFastReq(requestId, "c3-actor-fast"))
                .metadata("actor-id", actorId)
                .metadata(Contracts.SPOT_RID_METADATA, spotRid)
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.ActorRes.class);
        ScenarioAssert.that(actorId.equals(fast.actorId()), "ATD-C3 fast actor mismatch");
        Contracts.EvidenceRes evidence =
            ClientStreamSupport.waitForEvidence(connector, requestId, spotRid, "timer-await-completed");
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "timer-await-started",
            "timer-await-released",
            "actor-fast-started",
            "actor-fast-completed",
            "timer-await-resumed",
            "timer-await-completed");
        ScenarioAssert.that(
            evidence.markers().stream().anyMatch(entry -> entry.contains("marker=c3-actor-fast")),
            "ATD-C3 actor fast marker missing: " + List.copyOf(evidence.markers()));
    }
}
