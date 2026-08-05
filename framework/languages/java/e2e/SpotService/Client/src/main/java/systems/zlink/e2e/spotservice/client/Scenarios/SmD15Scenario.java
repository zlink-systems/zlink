package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD15Scenario extends SpotServiceScenarioContext {
    private SmD15Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD15Scenario(context).execute();
    }

    private void execute() {
        String actorId = "actor-sm-d15-" + UUID.randomUUID().toString().replace("-", "");
        String marker = "sm-d15-" + UUID.randomUUID().toString().replace("-", "");
        ZLinkStreamConnector connector = createStreamConnector(options().streamAEndpoint());
        try {
            Contracts.ActorProfile profile =
                new Contracts.ActorProfile("Push Chain", 15, List.of("gateway", "push"));
            connector.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = connector
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-D15 auth actor mismatch");
            ensure(auth.generation() > 0, "SM-D15 auth actor generation was not concrete");

            Contracts.ActorPingRes probe = connector
                .request(new Contracts.ActorPingReq("sm-d15-bind-probe"))
                .metadata("actor-id", actorId)
                .submit(Contracts.ActorPingRes.class).toCompletableFuture().join();
            ensure(actorId.equals(probe.actorId()), "SM-D15 bind probe actor mismatch");
            ensure("play-a".equals(probe.nodeRid()), "SM-D15 bind probe node mismatch");

            var pushed = connector.waitFor(Contracts.ActorPushNotify.class)
                .where(Contracts.ActorPushNotify.class, message ->
                    actorId.equals(message.payload().actorId())
                        && marker.equals(message.payload().value()))
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorPingRes reply = requestActorPush(actorId, marker);
            Contracts.ActorPushNotify notify = pushed.toCompletableFuture().join().payload();

            ensure(actorId.equals(reply.actorId()), "SM-D15 actor client reply actor mismatch");
            ensure("entry".equals(reply.spotRid()), "SM-D15 actor client reply spot mismatch");
            ensure("play-a".equals(reply.nodeRid()), "SM-D15 actor client reply node mismatch");
            ensure(marker.equals(reply.value()), "SM-D15 actor client reply marker mismatch");
            ensure(actorId.equals(notify.actorId()), "SM-D15 push actor mismatch");
            ensure("entry".equals(notify.spotRid()), "SM-D15 push spot mismatch");
            ensure(marker.equals(notify.value()), "SM-D15 push marker mismatch");

            waitForPlayAEvidence(List.of(
                "ActorPingReq|play-a|entry|" + actorId + "/sm-d15-bind-probe",
                "ActorPushReq|play-a|entry|" + actorId + "/" + marker));
            System.out.println("scenario SM-D15 passed");
        } catch (Exception error) {
            throw new IllegalStateException("actor push chain scenario failed", error);
        } finally {
            closeQuietly(connector);
        }

    }
}
