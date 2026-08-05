package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.UUID;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdB2SameActorReentryScenario {
    private AtdB2SameActorReentryScenario() {
    }

    public static void run(ZLinkStreamConnector connector, String actorId) {
        String requestId = "ATD-B2-" + UUID.randomUUID().toString().replace("-", "");
        CompletionStage<Contracts.ActorRes> await = connector
            .request(new Contracts.ActorAwaitReq(requestId, 350))
            .metadata("actor-id", actorId)
            .timeout(ClientStreamSupport.REQUEST_TIMEOUT)
            .submit(Contracts.ActorRes.class);
        ClientStreamSupport.sleep(75);
        CompletionStage<Contracts.ActorRes> fast = connector
            .request(new Contracts.ActorFastReq(requestId, "b2-fast"))
            .metadata("actor-id", actorId)
            .timeout(ClientStreamSupport.REQUEST_TIMEOUT)
            .submit(Contracts.ActorRes.class);
        Contracts.ActorRes awaitReply = await.toCompletableFuture().join();
        Contracts.ActorRes fastReply = fast.toCompletableFuture().join();
        ScenarioAssert.that(actorId.equals(awaitReply.actorId()), "ATD-B2 await actor mismatch");
        ScenarioAssert.that(actorId.equals(fastReply.actorId()), "ATD-B2 fast actor mismatch");
        Contracts.EvidenceRes evidence = ClientStreamSupport.evidence(connector, requestId);
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "actor-await-started",
            "actor-await-released",
            "actor-await-resumed",
            "actor-await-completed",
            "actor-fast-started",
            "actor-fast-completed");
        System.out.println("scenario ATD-B2 passed");
    }
}
