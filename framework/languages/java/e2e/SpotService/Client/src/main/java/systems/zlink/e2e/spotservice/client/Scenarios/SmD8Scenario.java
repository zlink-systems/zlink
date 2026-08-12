package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD8Scenario extends SpotServiceScenarioContext {
    private SmD8Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD8Scenario(context).execute();
    }

    private void execute() {
        String actorId = "actor-sm-d8-" + UUID.randomUUID().toString().replace("-", "");
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Reconnect", 8, List.of("reconnect"));
        ZLinkStreamConnector first = createStreamConnector(options().streamAEndpoint());
        try {
            first.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = first
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-D8 initial auth actor mismatch");

            var pending = first
                .request(new Contracts.SlowSessionReq("before-disconnect", 1_000))
                .timeout(Duration.ofSeconds(10))
                .submit(Contracts.SlowSessionRes.class);
            Thread.sleep(100);
            first.close().submit().toCompletableFuture().join();

            boolean pendingFailed = false;
            try {
                pending.toCompletableFuture().join();
            } catch (Exception ignored) {
                pendingFailed = true;
            }
            ensure(pendingFailed, "SM-D8 expected pending request to fail after stream disconnect");
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("stream reconnect scenario interrupted", error);
        } catch (Exception error) {
            throw new IllegalStateException("stream reconnect first phase failed", error);
        } finally {
            closeQuietly(first);
        }

        waitForPlayAEvidence(List.of("StreamDisconnected|play-a|session"));

        ZLinkStreamConnector second = createStreamConnector(options().streamAEndpoint());
        try {
            second.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = second
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-D8 reauth actor mismatch");

            Contracts.ActorEchoRes reply = second
                .request(new Contracts.ActorEchoReq("after-reconnect", 8, profile))
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            ensure(actorId.equals(reply.actorId()), "SM-D8 reconnected actor mismatch");
            ensure("play-a".equals(reply.nodeRid()), "SM-D8 reconnected node mismatch");
            ensure("entry:after-reconnect".equals(reply.value()), "SM-D8 reconnected value mismatch");

            System.out.println("scenario SM-D8 passed");
        } catch (Exception error) {
            throw new IllegalStateException("stream reconnect second phase failed", error);
        } finally {
            closeQuietly(second);
        }

    }
}
