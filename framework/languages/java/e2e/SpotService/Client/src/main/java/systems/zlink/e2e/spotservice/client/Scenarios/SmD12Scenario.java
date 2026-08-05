package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD12Scenario extends SpotServiceScenarioContext {
    private SmD12Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD12Scenario(context).execute();
    }

    private void execute() {
        String actorId = "actor-sm-d12-" + UUID.randomUUID().toString().replace("-", "");
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Transfer", 12, List.of("transfer"));
        ZLinkStreamConnector first = createStreamConnector(options().streamAEndpoint());
        int firstSeen;
        try {
            first.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = first
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-D12 first auth actor mismatch");
            ensure("play-a".equals(auth.nodeRid()), "SM-D12 first auth node mismatch");

            Contracts.ActorPingRes firstReply = first
                .request(new Contracts.ActorPingReq("before-transfer"))
                .submit(Contracts.ActorPingRes.class).toCompletableFuture().join();
            ensure(actorId.equals(firstReply.actorId()), "SM-D12 first reply actor mismatch");
            ensure("play-a".equals(firstReply.nodeRid()), "SM-D12 first reply node mismatch");
            ensure("before-transfer".equals(firstReply.value()), "SM-D12 first reply value mismatch");
            firstSeen = firstReply.seen();
            first.close().submit().toCompletableFuture().join();
        } catch (Exception error) {
            throw new IllegalStateException("stream rebind transfer first phase failed", error);
        } finally {
            closeQuietly(first);
        }

        waitForPlayAEvidence(List.of("StreamDisconnected|play-a|session"));

        ZLinkStreamConnector second = createStreamConnector(options().streamBEndpoint());
        try {
            second.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = second
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .timeout(Duration.ofSeconds(45))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-D12 second auth actor mismatch");
            ensure("play-a".equals(auth.nodeRid()), "SM-D12 second auth did not rebind existing actor");

            Contracts.SnapshotRes snapshot = second
                .request(new Contracts.SnapshotReq(actorId))
                .timeout(Duration.ofSeconds(15))
                .submit(Contracts.SnapshotRes.class).toCompletableFuture().join();
            ensure(actorId.equals(snapshot.actorId()), "SM-D12 snapshot actor mismatch");
            ensure(snapshot.seen() == firstSeen,
                "SM-D12 actor state was not preserved across stream servers");

            var pushed = second.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorPingRes resumed = second
                .request(new Contracts.ActorPushReq("after-transfer"))
                .timeout(Duration.ofSeconds(15))
                .submit(Contracts.ActorPingRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify notify = pushed.toCompletableFuture().join().payload();

            ensure(actorId.equals(resumed.actorId()), "SM-D12 resumed actor mismatch");
            ensure("play-a".equals(resumed.nodeRid()), "SM-D12 resumed node mismatch");
            ensure("after-transfer".equals(resumed.value()), "SM-D12 resumed value mismatch");
            ensure(resumed.seen() == firstSeen + 1,
                "SM-D12 resumed actor state mismatch");
            ensure(actorId.equals(notify.actorId()), "SM-D12 resumed push actor mismatch");
            ensure("after-transfer".equals(notify.value()), "SM-D12 resumed push value mismatch");
            ensure(notify.handlerSeq() == resumed.seen(), "SM-D12 push state mismatch");

            waitForPlayAEvidence(List.of("ActorPushReq|play-a|entry|" + actorId + "/after-transfer"));
            System.out.println("scenario SM-D12 passed");
        } catch (Exception error) {
            throw new IllegalStateException("stream rebind transfer second phase failed", error);
        } finally {
            closeQuietly(second);
        }

    }
}
