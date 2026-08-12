package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD6Scenario extends SpotServiceScenarioContext {
    private SmD6Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD6Scenario(context).execute();
    }

    private void execute() {
        String boundActorId = "actor-sm-d6-" + UUID.randomUUID().toString().replace("-", "");
        String shadowActorId = "actor-sm-d6-shadow-" + UUID.randomUUID().toString().replace("-", "");
        ZLinkStreamConnector bound = createStreamConnector(options().streamAEndpoint());
        ZLinkStreamConnector shadow = createStreamConnector(options().streamBEndpoint());
        Contracts.ActorProfile boundProfile = new Contracts.ActorProfile("Bound", 6, List.of("bound"));
        Contracts.ActorProfile shadowProfile = new Contracts.ActorProfile("Shadow", 6, List.of("shadow"));
        try {
            bound.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes boundAuth = bound
                .request(new Contracts.ActorAuthReq(boundActorId, boundProfile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(boundActorId.equals(boundAuth.actorId()), "SM-D6 bound auth actor mismatch");

            shadow.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes shadowAuth = shadow
                .request(new Contracts.ActorAuthReq(shadowActorId, shadowProfile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(shadowActorId.equals(shadowAuth.actorId()), "SM-D6 shadow auth actor mismatch");

            var shadowPush = shadow.expectNone(Contracts.ActorPushNotify.class)
                .within(Duration.ofMillis(400))
                .submit();
            var boundPush = bound.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorEchoRes reply = bound
                .request(new Contracts.ActorEchoReq("push-bound-only", 20, boundProfile))
                .metadata("actor-id", boundActorId)
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify notify = boundPush.toCompletableFuture().join().payload();

            ensure(boundActorId.equals(reply.actorId()), "SM-D6 reply actor mismatch");
            ensure(boundActorId.equals(notify.actorId()), "SM-D6 push actor mismatch");
            ensure("push:push-bound-only".equals(notify.value()), "SM-D6 push value mismatch");
            shadowPush.toCompletableFuture().join();

            waitForPlayAEvidence(List.of("ActorEntryReq|play-a|entry|" + boundActorId + "/push-bound-only"));
            System.out.println("scenario SM-D6 passed");
        } catch (Exception error) {
            throw new IllegalStateException("bound session push isolation scenario failed", error);
        } finally {
            closeQuietly(bound);
            closeQuietly(shadow);
        }

    }
}
