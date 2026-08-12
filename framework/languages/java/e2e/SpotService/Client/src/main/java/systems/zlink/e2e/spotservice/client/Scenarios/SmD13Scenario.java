package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD13Scenario extends SpotServiceScenarioContext {
    private SmD13Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD13Scenario(context).execute();
    }

    private void execute() {
        String actorId = "actor-sm-d13-" + UUID.randomUUID().toString().replace("-", "");
        ZLinkStreamConnector connector = createStreamConnector(options().streamAEndpoint());
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Heartbeat", 13, List.of("heartbeat"));
        try {
            connector.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = connector
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-D13 auth actor mismatch");

            Thread.sleep(1_500);
            ensure(connector.isConnected(), "SM-D13 heartbeat-enabled stream disconnected");

            Contracts.ActorEchoRes reply = connector
                .request(new Contracts.ActorEchoReq("after-heartbeat", 13, profile))
                .metadata("actor-id", actorId)
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            ensure(actorId.equals(reply.actorId()), "SM-D13 actor reply mismatch");
            ensure("play-a".equals(reply.nodeRid()), "SM-D13 actor node mismatch");
            ensure("entry:after-heartbeat".equals(reply.value()), "SM-D13 actor value mismatch");

            waitForPlayAEvidence(List.of("StreamInbound|play-a|session|ActorEchoReq"));
            System.out.println("scenario SM-D13 passed");
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("stream heartbeat scenario interrupted", error);
        } catch (Exception error) {
            throw new IllegalStateException("stream heartbeat scenario failed", error);
        } finally {
            closeQuietly(connector);
        }

    }
}
