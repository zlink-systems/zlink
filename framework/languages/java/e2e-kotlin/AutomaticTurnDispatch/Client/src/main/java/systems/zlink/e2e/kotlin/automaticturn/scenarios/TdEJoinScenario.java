package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.UUID;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class TdEJoinScenario {
    private TdEJoinScenario() {
    }

    public static void runUserSpotJoin(ZLinkStreamConnector connector) {
        Fixture fixture = prepare(connector, "TD-E2");
        Contracts.ActorJoinRes joined = ClientStreamSupport.joinActor(
            connector, fixture.actorA(), fixture.spotB(), "td-e2");
        ScenarioAssert.that(
            fixture.actorA().equals(joined.actorId()),
            "TD-E2 joined actor mismatch");
        ScenarioAssert.that(
            fixture.spotB().equals(joined.spotRid()),
            "TD-E2 target spot mismatch");
        System.out.println("scenario TD-E2 passed");
    }

    public static void runOppositeUserSpotJoins(ZLinkStreamConnector connector) {
        Fixture fixture = prepare(connector, "TD-E3");
        ClientStreamSupport.joinActor(
            connector, fixture.actorB(), fixture.spotB(), "td-e3-prepare");

        CompletionStage<Contracts.ActorJoinRes> aToB = requestJoin(
            connector, fixture.actorA(), fixture.spotB(), "td-e3-a-to-b");
        CompletionStage<Contracts.ActorJoinRes> bToA = requestJoin(
            connector, fixture.actorB(), fixture.spotA(), "td-e3-b-to-a");
        Contracts.ActorJoinRes joinedA = aToB.toCompletableFuture().join();
        Contracts.ActorJoinRes joinedB = bToA.toCompletableFuture().join();
        ScenarioAssert.that(
            fixture.actorA().equals(joinedA.actorId()) && fixture.spotB().equals(joinedA.spotRid()),
            "TD-E3 A to B result mismatch");
        ScenarioAssert.that(
            fixture.actorB().equals(joinedB.actorId()) && fixture.spotA().equals(joinedB.spotRid()),
            "TD-E3 B to A result mismatch");
        System.out.println("scenario TD-E3 passed");
    }

    private static Fixture prepare(ZLinkStreamConnector connector, String scenarioId) {
        String id = scenarioId.toLowerCase().replace("-", "") + "-"
            + UUID.randomUUID().toString().replace("-", "");
        String spotA = id + "-spot-a";
        String spotB = id + "-spot-b";
        String actorA = id + "-actor-a";
        String actorB = id + "-actor-b";
        ensureSpot(connector, spotA, scenarioId);
        ensureSpot(connector, spotB, scenarioId);
        Contracts.BindActorsRes bound = ClientStreamSupport.bindActors(
            connector, spotA, actorA, actorB);
        ScenarioAssert.that(actorA.equals(bound.actorA()), scenarioId + " actor A bind mismatch");
        ScenarioAssert.that(actorB.equals(bound.actorB()), scenarioId + " actor B bind mismatch");
        ClientStreamSupport.joinActor(connector, actorA, spotA, scenarioId + "-prepare-a");
        ClientStreamSupport.joinActor(connector, actorB, spotA, scenarioId + "-prepare-b");
        return new Fixture(spotA, spotB, actorA, actorB);
    }

    private static void ensureSpot(
        ZLinkStreamConnector connector,
        String spotRid,
        String scenarioId) {
        Contracts.EnsureSpotRes ensured = ClientStreamSupport.await(
            connector.request(new Contracts.EnsureSpotReq(spotRid))
                .metadata(Contracts.TARGET_NODE_RID_METADATA, "play-a")
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.EnsureSpotRes.class);
        ScenarioAssert.that(
            spotRid.equals(ensured.spotRid()),
            scenarioId + " spot creation mismatch: " + spotRid);
    }

    private static CompletionStage<Contracts.ActorJoinRes> requestJoin(
        ZLinkStreamConnector connector,
        String actorId,
        String spotRid,
        String value) {
        return connector.request(new Contracts.ActorJoinReq(spotRid, value))
            .metadata("actor-id", actorId)
            .timeout(ClientStreamSupport.REQUEST_TIMEOUT)
            .submit(Contracts.ActorJoinRes.class);
    }

    private record Fixture(String spotA, String spotB, String actorA, String actorB) {
    }
}
