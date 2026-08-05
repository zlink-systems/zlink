package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import java.util.List;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmB1Scenario extends SpotServiceScenarioContext {
    private SmB1Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmB1Scenario(context).execute();
    }

    private void execute() {
        ZLinkStreamConnector connector = createStreamConnector(options().streamAEndpoint());
        ZLinkStreamConnector unbound = createStreamConnector(options().streamAEndpoint());
        try {
            Contracts.ActorProfile profile = new Contracts.ActorProfile(
                "Player One",
                7,
                List.of("alpha", "beta"));
            connector.connect().submit().toCompletableFuture().join();
            unbound.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = connector
                .request(new Contracts.ActorAuthReq("actor-local-1", profile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure("actor-local-1".equals(auth.actorId()), "SM-D1 auth actor mismatch");
            ensure(auth.boundCount() == 1, "SM-D1 bound actor count mismatch");
            ensure(profile.displayName().equals(auth.displayName()), "SM-B3 create profile display name mismatch");
            ensure(profile.level() == auth.level(), "SM-B3 create profile level mismatch");
            ensure(profile.tags().equals(auth.tags()), "SM-B3 create profile tags mismatch");

            var entryPush = connector.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorEchoRes entryReply = connector
                .request(new Contracts.ActorEchoReq("entry-echo", 1, profile))
                .metadata("actor-id", "actor-local-1")
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify entry = entryPush.toCompletableFuture().join().payload();
            ensure("entry:entry-echo".equals(entryReply.value()), "SM-B1 entry actor request mismatch");
            ensure("entry".equals(entryReply.spotRid()), "SM-D3 entry bind spot mismatch");
            ensure("entry".equals(entry.spotRid()), "SM-D3 entry push spot mismatch");
            ensure("actor-local-1".equals(entryReply.actorId()), "SM-D3 entry bind actor mismatch");
            ensure("actor-local-1".equals(entry.actorId()), "SM-D3 entry push actor mismatch");
            ensure(entryReply.requestSeq() == 1, "SM-B3 entry request sequence mismatch");
            ensure(profile.displayName().equals(entryReply.displayName()), "SM-B3 entry profile display name mismatch");
            ensure(profile.level() == entryReply.level(), "SM-B3 entry profile level mismatch");
            ensure(profile.tags().equals(entryReply.tags()), "SM-B3 entry profile tags mismatch");
            ensure(entry.requestSeq() == 1, "SM-D1 entry push request sequence mismatch");
            ensure("push:entry-echo".equals(entry.value()), "SM-D1 entry push mismatch");

            Contracts.ActorJoinRes joined = connector
                .request(new Contracts.ActorJoinReq("room-a", profile, profile.tags()))
                .metadata("actor-id", "actor-local-1")
                .submit(Contracts.ActorJoinRes.class).toCompletableFuture().join();
            ensure("room-a".equals(joined.spotRid()), "SM-B1 joined spot mismatch");
            ensure(profile.tags().equals(joined.tags()), "SM-B3 join payload tags mismatch");
            ensure(profile.displayName().equals(joined.displayName()), "SM-B3 join payload display name mismatch");
            ensure(profile.level() == joined.level(), "SM-B3 join payload level mismatch");

            var unboundPush = unbound.waitFor(Contracts.ActorPushNotify.class)
                .timeout(Duration.ofMillis(400))
                .submit(Contracts.ActorPushNotify.class);
            var userPush1 = connector.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorEchoRes userReply1 = connector
                .request(new Contracts.ActorEchoReq("user-echo-1", 2, profile))
                .metadata("actor-id", "actor-local-1")
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify user1 = userPush1.toCompletableFuture().join().payload();
            ensure("room-a".equals(userReply1.spotRid()), "SM-B1 user actor spot mismatch");
            ensure("room-a".equals(joined.spotRid()), "SM-D3 user bind spot mismatch");
            ensure("room-a".equals(user1.spotRid()), "SM-D3 user push spot mismatch");
            ensure("actor-local-1".equals(userReply1.actorId()), "SM-D3 user relay actor mismatch");
            ensure("actor-local-1".equals(user1.actorId()), "SM-D3 user push actor mismatch");
            ensure("user:user-echo-1".equals(userReply1.value()), "SM-B1 user actor request mismatch");
            ensure(userReply1.requestSeq() == 2, "SM-B3 user request sequence mismatch");
            ensure(profile.displayName().equals(userReply1.displayName()), "SM-B3 user profile display name mismatch");
            ensure(profile.level() == userReply1.level(), "SM-B3 user profile level mismatch");
            ensure(profile.tags().equals(userReply1.tags()), "SM-B3 user profile tags mismatch");
            ensure(user1.requestSeq() == 2, "SM-D1 user push request sequence mismatch");
            ensure("push:user-echo-1".equals(user1.value()), "SM-D1 user push mismatch");
            expectFailure(() -> awaitUnchecked(unbound, unboundPush));

            var userPush2 = connector.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorEchoRes userReply2 = connector
                .request(new Contracts.ActorEchoReq("user-echo-2", 3, profile))
                .metadata("actor-id", "actor-local-1")
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify user2 = userPush2.toCompletableFuture().join().payload();
            ensure(userReply2.requestSeq() == 3, "SM-B7 second packet request sequence mismatch");
            ensure(user2.requestSeq() == 3, "SM-B7 second push request sequence mismatch");

            var userPush3 = connector.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorEchoRes userReply3 = connector
                .request(new Contracts.ActorEchoReq("user-echo-3", 4, profile))
                .metadata("actor-id", "actor-local-1")
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify user3 = userPush3.toCompletableFuture().join().payload();
            ensure(userReply3.requestSeq() == 4, "SM-B7 third packet request sequence mismatch");
            ensure(user3.requestSeq() == 4, "SM-B7 third push request sequence mismatch");
            ensure(userReply1.handlerSeq() < userReply2.handlerSeq()
                    && userReply2.handlerSeq() < userReply3.handlerSeq(),
                "SM-B7 actor packet handler sequence was not preserved");
            ensure(user1.handlerSeq() < user2.handlerSeq()
                    && user2.handlerSeq() < user3.handlerSeq(),
                "SM-B7 actor push sequence was not preserved");
            waitForPlayAEvidence(List.of("StreamInbound|play-a|session|ActorAuthReq"));

            System.out.println("scenario SM-B1 passed");
            System.out.println("scenario SM-B3 passed");
            System.out.println("scenario SM-B7 passed");
            System.out.println("scenario SM-D1 passed");
            System.out.println("scenario SM-D3 passed");
            System.out.println("scenario SM-D9 passed");
        } catch (Exception error) {
            throw new IllegalStateException("actor/session scenario failed", error);
        } finally {
            try {
                connector.close().submit().toCompletableFuture().join();
            } catch (Exception ignored) {
            }
            try {
                unbound.close().submit().toCompletableFuture().join();
            } catch (Exception ignored) {
            }
        }

    }
}
