package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;

public final class SmD10Scenario extends SpotServiceScenarioContext {
    private SmD10Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD10Scenario(context).execute();
    }

    private void execute() {
        String congestedActorId = "actor-sm-d10-congested-" + UUID.randomUUID().toString().replace("-", "");
        String isolatedActorId = "actor-sm-d10-isolated-" + UUID.randomUUID().toString().replace("-", "");
        Contracts.ActorProfile congestedProfile =
            new Contracts.ActorProfile("Backpressure", 10, List.of("congested"));
        Contracts.ActorProfile isolatedProfile =
            new Contracts.ActorProfile("Backpressure Peer", 10, List.of("isolated"));
        ZLinkStreamConnector congested = createStreamConnector(
            options().streamAEndpoint(),
            ZLinkStreamDispatchMode.MANUAL,
            1);
        ZLinkStreamConnector isolated = createStreamConnector(options().streamBEndpoint());
        try {
            congested.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes congestedAuth = congested
                .request(new Contracts.ActorAuthReq(congestedActorId, congestedProfile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(congestedActorId.equals(congestedAuth.actorId()), "SM-D10 congested auth actor mismatch");

            isolated.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes isolatedAuth = isolated
                .request(new Contracts.ActorAuthReq(isolatedActorId, isolatedProfile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(isolatedActorId.equals(isolatedAuth.actorId()), "SM-D10 isolated auth actor mismatch");

            var retainedPush = congested.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            for (int index = 0; index < 8; index++) {
                Contracts.ActorEchoRes reply = congested
                    .request(new Contracts.ActorEchoReq("burst-" + index, 10, congestedProfile))
                    .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
                ensure(congestedActorId.equals(reply.actorId()), "SM-D10 congested reply actor mismatch");
            }
            ensure(congested.receivedCount("ActorPushNotify") <= 1,
                "SM-D10 congested queue retained too many pushes");
            congested.dispatch().submit().toCompletableFuture().join();
            Contracts.ActorPushNotify retained = retainedPush.toCompletableFuture().join().payload();
            ensure(congestedActorId.equals(retained.actorId()), "SM-D10 retained push actor mismatch");
            ensure("push:burst-7".equals(retained.value()),
                "SM-D10 expected newest congested push to be retained");

            Contracts.ActorEchoRes stillAlive = congested
                .request(new Contracts.ActorEchoReq("after-backpressure", 10, congestedProfile))
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            ensure(congestedActorId.equals(stillAlive.actorId()), "SM-D10 congested session stopped routing");
            ensure("entry:after-backpressure".equals(stillAlive.value()),
                "SM-D10 congested session reply mismatch");
            congested.dispatch().submit().toCompletableFuture().join();

            var isolatedPush = isolated.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorEchoRes isolatedReply = isolated
                .request(new Contracts.ActorEchoReq("isolated-push", 10, isolatedProfile))
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify isolatedNotify = isolatedPush.toCompletableFuture().join().payload();
            ensure(isolatedActorId.equals(isolatedReply.actorId()), "SM-D10 isolated reply actor mismatch");
            ensure(isolatedActorId.equals(isolatedNotify.actorId()), "SM-D10 isolated push actor mismatch");
            ensure("push:isolated-push".equals(isolatedNotify.value()),
                "SM-D10 isolated session push mismatch");

            System.out.println("scenario SM-D10 passed");
        } catch (Exception error) {
            throw new IllegalStateException("stream backpressure scenario failed", error);
        } finally {
            closeQuietly(congested);
            closeQuietly(isolated);
        }

    }
}
