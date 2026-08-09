package Scenarios;

import systems.zlink.e2e.spotservice.client.Scenarios;
import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD5AScenario extends SpotServiceScenarioContext {
    private SmD5AScenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD5AScenario(context).execute();
    }

    private void execute() {
        String suffix = UUID.randomUUID().toString().replace("-", "");
        String selected = "actor-sm-d5a-selected-" + suffix;
        String other = "actor-sm-d5a-other-" + suffix;
        Contracts.ActorProfile profile =
            new Contracts.ActorProfile("SM-D5A", 5, List.of("logical-disconnect"));
        ZLinkStreamConnector connector = createStreamConnector(options().streamAEndpoint());
        try {
            connector.connect().submit().toCompletableFuture().join();
            Contracts.MultiBindRes bound = connector
                .request(new Contracts.MultiBindReq(selected, other, profile))
                .submit(Contracts.MultiBindRes.class).toCompletableFuture().join();
            ensure(bound.boundCount() == 2, "SM-D5A expected two bound actors");
            joinUserSpot(connector, selected, profile);
            joinUserSpot(connector, other, profile);
            Contracts.NotifyBoundActorDisconnectedRes notified = connector
                .request(new Contracts.NotifyBoundActorDisconnectedReq(selected))
                .submit(Contracts.NotifyBoundActorDisconnectedRes.class)
                .toCompletableFuture().join();
            ensure(notified.completed() && selected.equals(notified.actorId()),
                "SM-D5A logical disconnect did not complete for the selected Actor");
            Contracts.ActorEchoRes otherReply = connector
                .request(new Contracts.ActorEchoReq("still-connected", 1, profile))
                .metadata("actor-id", other)
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            ensure(other.equals(otherReply.actorId())
                    && "user:still-connected".equals(otherReply.value()),
                "SM-D5A logical disconnect changed the other binding");
            Contracts.EvidenceSnapshot evidence = waitForPlayAEvidence(List.of(
                "ActorUserDisconnected|play-a|room-a|" + selected));
            ensure(countActorEvidence(evidence, "ActorUserDisconnected", "room-a", selected) == 1,
                "SM-D5A selected Actor callback count was not one");
            ensure(countActorEvidence(evidence, "ActorUserDisconnected", "room-a", other) == 0,
                "SM-D5A notified the unselected Actor");
            System.out.println("scenario SM-D5A passed");
        } finally {
            closeQuietly(connector);
        }
    }

    private void joinUserSpot(
        ZLinkStreamConnector connector,
        String actorId,
        Contracts.ActorProfile profile) {
        Contracts.ActorJoinRes joined = connector
            .request(new Contracts.ActorJoinReq("room-a", profile, profile.tags()))
            .metadata("actor-id", actorId)
            .submit(Contracts.ActorJoinRes.class).toCompletableFuture().join();
        ensure(actorId.equals(joined.actorId()) && "room-a".equals(joined.spotRid()),
            "SM-D5A Actor did not join the selected User Spot");
    }
}
