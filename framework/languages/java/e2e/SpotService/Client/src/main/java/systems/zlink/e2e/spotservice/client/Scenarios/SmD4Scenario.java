package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD4Scenario extends SpotServiceScenarioContext {
    private SmD4Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD4Scenario(context).execute();
    }

    private void execute() {
        String firstActorId = "actor-sm-d4-x-" + UUID.randomUUID().toString().replace("-", "");
        String secondActorId = "actor-sm-d4-y-" + UUID.randomUUID().toString().replace("-", "");
        ZLinkStreamConnector connector = createStreamConnector(options().streamAEndpoint());
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Multi Bind", 9, List.of("multi", "bind"));
        try {
            connector.connect().submit().toCompletableFuture().join();
            Contracts.MultiBindRes bound = connector
                .request(new Contracts.MultiBindReq(firstActorId, secondActorId, profile))
                .submit(Contracts.MultiBindRes.class).toCompletableFuture().join();
            ensure(bound.boundCount() == 2, "SM-D4 expected two bound actors");

            Contracts.ActorEchoRes first = connector
                .request(new Contracts.ActorEchoReq("to-x", 10, profile))
                .metadata("actor-id", firstActorId)
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorEchoRes second = connector
                .request(new Contracts.ActorEchoReq("to-y", 11, profile))
                .metadata("actor-id", secondActorId)
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            ensure(firstActorId.equals(first.actorId()) && "entry:to-x".equals(first.value()),
                "SM-D4 first actor relay mismatch");
            ensure(secondActorId.equals(second.actorId()) && "entry:to-y".equals(second.value()),
                "SM-D4 second actor relay mismatch");

            var firstPushed = connector.waitFor(Contracts.ActorPushNotify.class)
                .where(Contracts.ActorPushNotify.class, message -> firstActorId.equals(message.payload().actorId()))
                .submit(Contracts.ActorPushNotify.class);
            connector
                .request(new Contracts.ActorEchoReq("push-x", 12, profile))
                .metadata("actor-id", firstActorId)
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify firstPush = firstPushed.toCompletableFuture().join().payload();
            ensure(firstActorId.equals(firstPush.actorId()) && "push:push-x".equals(firstPush.value()),
                "SM-D4 first actor push mismatch");

            var secondPushed = connector.waitFor(Contracts.ActorPushNotify.class)
                .where(Contracts.ActorPushNotify.class, message -> secondActorId.equals(message.payload().actorId()))
                .submit(Contracts.ActorPushNotify.class);
            connector
                .request(new Contracts.ActorEchoReq("push-y", 13, profile))
                .metadata("actor-id", secondActorId)
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify secondPush = secondPushed.toCompletableFuture().join().payload();
            ensure(secondActorId.equals(secondPush.actorId()) && "push:push-y".equals(secondPush.value()),
                "SM-D4 second actor push mismatch");

            expectFailure(() -> {
                try {
                    connector
                        .request(new Contracts.ActorEchoReq("missing-actor-id", 14, profile))
                        .timeout(Duration.ofSeconds(2))
                        .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
                } catch (Exception error) {
                    throw new RuntimeException(error);
                }
            });

            waitForPlayAEvidence(List.of(
                "ActorSessionBound|play-a|session|" + firstActorId,
                "ActorSessionBound|play-a|session|" + secondActorId));
            System.out.println("scenario SM-D4 passed");
        } catch (Exception error) {
            throw new IllegalStateException("multi actor bind scenario failed", error);
        } finally {
            closeQuietly(connector);
        }

    }
}
