package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmB2Scenario extends SpotServiceScenarioContext {
    private SmB2Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmB2Scenario(context).execute();
    }

    private void execute() {
        String actorId = "actor-sm-remote-" + UUID.randomUUID().toString().replace("-", "");
        ZLinkStreamConnector connector = createStreamConnector(options().streamBEndpoint());
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Remote Player", 24, List.of("remote", "relay"));
        try {
            connector.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = connector
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-B2 remote auth actor mismatch");
            ensure("play-b".equals(auth.nodeRid()), "SM-B2 remote actor was not created on play-b");
            ensure(auth.generation() > 0, "SM-B2 remote auth actor generation was not concrete");

            Contracts.ActorJoinRes joined = connector
                .request(new Contracts.ActorJoinReq("room-a", profile, profile.tags()))
                .metadata("actor-id", actorId)
                .timeout(Duration.ofSeconds(15))
                .submit(Contracts.ActorJoinRes.class).toCompletableFuture().join();
            ensure(actorId.equals(joined.actorId()), "SM-B2 remote join actor mismatch");
            ensure("room-a".equals(joined.spotRid()), "SM-B2 remote join spot mismatch");
            ensure("play-a".equals(joined.nodeRid()), "SM-B2 remote join did not cross to play-a");

            var userPush = connector.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorEchoRes userReply = connector
                .request(new Contracts.ActorEchoReq("remote-user-echo", 42, profile))
                .metadata("actor-id", actorId)
                .timeout(Duration.ofSeconds(15))
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify push = userPush.toCompletableFuture().join().payload();
            ensure("user:remote-user-echo".equals(userReply.value()), "SM-B4 remote actor reply mismatch");
            ensure("room-a".equals(userReply.spotRid()), "SM-B4 remote actor reply spot mismatch");
            ensure("play-a".equals(userReply.nodeRid()), "SM-B4 remote actor reply node mismatch");
            ensure("push:remote-user-echo".equals(push.value()), "SM-D2 remote bound-session push mismatch");
            ensure(actorId.equals(push.actorId()), "SM-D2 remote bound-session push actor mismatch");

            waitForPlayBEvidence(List.of("ActorCreated|play-b|entry|" + actorId));
            waitForPlayAEvidence(List.of(
                "ActorUserJoined|play-a|room-a|" + actorId,
                "ActorUserReq|play-a|room-a|" + actorId + "/remote-user-echo"));

            System.out.println("scenario SM-B2 passed");
            System.out.println("scenario SM-B4 passed");
            System.out.println("scenario SM-D2 passed");
        } catch (Exception error) {
            throw new IllegalStateException("remote actor/session scenario failed", error);
        } finally {
            closeQuietly(connector);
        }

    }
}
