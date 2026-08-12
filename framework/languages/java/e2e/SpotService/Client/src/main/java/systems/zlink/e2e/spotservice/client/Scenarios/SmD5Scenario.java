package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD5Scenario extends SpotServiceScenarioContext {
    private SmD5Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD5Scenario(context).execute();
    }

    private void execute() {
        try {
            String actorId = "actor-sm-d5-notified-" + UUID.randomUUID().toString().replace("-", "");
            ZLinkStreamConnector connector = createStreamConnector(options().streamAEndpoint());
            Contracts.ActorProfile profile = new Contracts.ActorProfile("Disconnect", 5, List.of("disconnect"));
            try {
                connector.connect().submit().toCompletableFuture().join();
                Contracts.ActorAuthRes auth = connector
                    .request(new Contracts.ActorAuthReq(actorId, profile))
                    .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
                ensure(actorId.equals(auth.actorId()), "SM-D5 auth actor mismatch");
                Contracts.ActorJoinRes joined = connector
                    .request(new Contracts.ActorJoinReq("room-a", profile, profile.tags()))
                    .metadata("actor-id", actorId)
                    .submit(Contracts.ActorJoinRes.class).toCompletableFuture().join();
                ensure(actorId.equals(joined.actorId()), "SM-D5 join actor mismatch");
            } finally {
                closeQuietly(connector);
            }

            Contracts.EvidenceSnapshot evidence = waitForPlayAEvidence(
                List.of("ActorUserDisconnected|play-a|room-a|" + actorId));
            ensure(evidence.entries().stream().anyMatch(entry ->
                    "ActorUserDisconnected".equals(entry.marker()) && actorId.equals(entry.value())),
                "SM-D5 expected selected actor disconnect callback evidence");

            System.out.println("scenario SM-D5 passed");
        } catch (Exception error) {
            throw new IllegalStateException("actor disconnect notify scenario failed", error);
        }

    }
}
