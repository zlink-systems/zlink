package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.UUID;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdB1OtherActorProgressScenario {
    private AtdB1OtherActorProgressScenario() {
    }

    public static JoinedActors run(ZLinkStreamConnector roomA, ZLinkStreamConnector roomB) {
        Contracts.BindActorsRes bind = ClientStreamSupport.bindActors(
            roomA,
            "room-a",
            "actor-room-a",
            "actor-room-b");
        ScenarioAssert.that("actor-room-a".equals(bind.actorA()), "ATD-B1 bind actor A mismatch");
        ScenarioAssert.that("actor-room-b".equals(bind.actorB()), "ATD-B1 bind actor B mismatch");
        ClientStreamSupport.bindActors(
            roomB,
            "room-a",
            "actor-room-a",
            "actor-room-b");
        Contracts.ActorJoinRes joinedA = ClientStreamSupport.joinActor(
            roomA,
            "actor-room-a",
            "room-a",
            "initial-a");
        ScenarioAssert.that("room-a".equals(joinedA.spotRid()), "ATD-B1 join spot mismatch");
        ScenarioAssert.that("joined:initial-a".equals(joinedA.value()), "ATD-B1 join await reply mismatch");
        Contracts.ActorJoinRes joinedB = ClientStreamSupport.joinActor(
            roomB,
            "actor-room-b",
            "room-b",
            "initial-b");
        ScenarioAssert.that("room-b".equals(joinedB.spotRid()), "ATD-B1 join actor B spot mismatch");
        ScenarioAssert.that("joined:initial-b".equals(joinedB.value()), "ATD-B1 join actor B reply mismatch");
        String requestId = "ATD-B1-" + UUID.randomUUID().toString().replace("-", "");
        CompletionStage<Contracts.ActorRes> await = roomA
            .request(new Contracts.ActorAwaitReq(requestId, 1200))
            .metadata("actor-id", "actor-room-a")
            .timeout(ClientStreamSupport.REQUEST_TIMEOUT)
            .submit(Contracts.ActorRes.class);
        ClientStreamSupport.sleep(250);
        CompletionStage<Contracts.ActorRes> fast = roomB
            .request(new Contracts.ActorFastReq(requestId, "b1-fast"))
            .metadata("actor-id", "actor-room-b")
            .timeout(ClientStreamSupport.REQUEST_TIMEOUT)
            .submit(Contracts.ActorRes.class);
        Contracts.ActorRes fastReply = fast.toCompletableFuture().join();
        Contracts.ActorRes awaitReply = await.toCompletableFuture().join();
        ScenarioAssert.that("actor-room-a".equals(awaitReply.actorId()), "ATD-B1 await actor mismatch");
        ScenarioAssert.that("actor-room-b".equals(fastReply.actorId()), "ATD-B1 fast actor mismatch");
        Contracts.EvidenceRes evidence = ClientStreamSupport.evidence(roomA, requestId);
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "actor-await-started",
            "actor-await-released",
            "actor-fast-started",
            "actor-fast-completed",
            "actor-await-resumed",
            "actor-await-completed");
        System.out.println("scenario ATD-B1 passed");
        return new JoinedActors("actor-room-a", "actor-room-b");
    }

    public record JoinedActors(String actorA, String actorB) {
    }
}
