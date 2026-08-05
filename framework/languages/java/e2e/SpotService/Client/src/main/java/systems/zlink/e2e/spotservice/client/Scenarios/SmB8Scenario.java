package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmB8Scenario extends SpotServiceScenarioContext {
    private SmB8Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmB8Scenario(context).execute();
    }

    private void execute() {
        String actorId = "actor-sm-b8-destroy-" + UUID.randomUUID().toString().replace("-", "");
        ZLinkStreamConnector connector = createStreamConnector(options().streamAEndpoint());
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Destroy", 8, List.of("destroy"));
        try {
            connector.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = connector
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .timeout(Duration.ofSeconds(15))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-B8 auth actor mismatch");

            Contracts.ActorDestroyRes destroy = connector
                .request(new Contracts.ActorDestroyReq(actorId))
                .metadata("actor-id", actorId)
                .submit(Contracts.ActorDestroyRes.class).toCompletableFuture().join();
            ensure(actorId.equals(destroy.actorId()), "SM-B8 destroy actor mismatch");
            ensure(destroy.destroyed(), "SM-B8 destroy was not accepted");

            expectFailure(() -> {
                try {
                    connector
                        .request(new Contracts.ActorEchoReq("after-destroy", 1, profile))
                        .metadata("actor-id", actorId)
                        .timeout(Duration.ofMillis(500))
                        .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
                } catch (Exception error) {
                    throw new RuntimeException(error);
                }
            });

            Contracts.EvidenceSnapshot evidence = waitForPlayAEvidence(
                List.of("ActorDestroyed|play-a|entry|" + actorId));
            long destroyedCount = evidence.entries().stream()
                .filter(entry -> "ActorDestroyed".equals(entry.marker()) && actorId.equals(entry.value()))
                .count();
            ensure(destroyedCount == 1, "SM-B8 destroy evidence count mismatch");

            System.out.println("scenario SM-B8 passed");
        } catch (Exception error) {
            throw new IllegalStateException("actor destroy scenario failed", error);
        } finally {
            closeQuietly(connector);
        }

    }
}
