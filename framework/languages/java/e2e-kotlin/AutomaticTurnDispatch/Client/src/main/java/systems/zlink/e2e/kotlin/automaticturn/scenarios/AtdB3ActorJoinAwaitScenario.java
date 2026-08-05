package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.UUID;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdB3ActorJoinAwaitScenario {
    private AtdB3ActorJoinAwaitScenario() {
    }

    public static void run(
        ZLinkStreamConnector joinConnector,
        String joiningActorId,
        ZLinkStreamConnector fastConnector,
        String fastActorId) {
        String requestId = "ATD-B3-" + UUID.randomUUID().toString().replace("-", "");
        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        Contracts.BindActorsRes joinedSession = ClientStreamSupport.bindActors(
            joinConnector,
            "room-a",
            actorA,
            actorB);
        ScenarioAssert.that(actorA.equals(joinedSession.actorA()), "ATD-B3 actor A bind mismatch");
        ScenarioAssert.that(actorB.equals(joinedSession.actorB()), "ATD-B3 actor B bind mismatch");
        ClientStreamSupport.bindActors(
            fastConnector,
            "room-a",
            actorA,
            actorB);
        CompletionStage<Contracts.ActorRes> join = joinConnector
            .request(new Contracts.ActorJoinAwaitReq(requestId, "room-a"))
            .metadata("actor-id", actorA)
            .timeout(ClientStreamSupport.REQUEST_TIMEOUT)
            .submit(Contracts.ActorRes.class);
        ClientStreamSupport.sleep(75);
        CompletionStage<Contracts.ActorRes> fast = fastConnector
            .request(new Contracts.ActorFastReq(requestId, "b3-fast"))
            .metadata("actor-id", actorB)
            .timeout(ClientStreamSupport.REQUEST_TIMEOUT)
            .submit(Contracts.ActorRes.class);
        Contracts.ActorRes fastReply = fast.toCompletableFuture().join();
        Contracts.ActorRes joinReply = join.toCompletableFuture().join();
        ScenarioAssert.that(actorA.equals(joinReply.actorId()), "ATD-B3 join actor mismatch");
        ScenarioAssert.that(actorB.equals(fastReply.actorId()), "ATD-B3 fast actor mismatch");
        Contracts.EvidenceRes evidence = ClientStreamSupport.evidence(joinConnector, requestId);
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "actor-join-await-started",
            "actor-join-await-released",
            "actor-fast-started",
            "actor-fast-completed",
            "actor-join-await-resumed",
            "actor-join-await-completed");
        System.out.println("scenario ATD-B3 passed");
    }
}
