package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD7Scenario extends SpotServiceScenarioContext {
    private SmD7Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD7Scenario(context).execute();
    }

    private void execute() {
        ZLinkStreamConnector preAuth = createStreamConnector(options().streamAEndpoint());
        Contracts.ActorProfile preAuthProfile =
            new Contracts.ActorProfile("PreAuth", 7, List.of("pre-auth"));
        try {
            preAuth.connect().submit().toCompletableFuture().join();
            expectFailure(() -> {
                try {
                    preAuth
                        .request(new Contracts.ActorEchoReq("pre-auth-dispatch", 7, preAuthProfile))
                        .timeout(Duration.ofMillis(500))
                        .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
                } catch (Exception error) {
                    throw new RuntimeException(error);
                }
            });
        } catch (Exception error) {
            throw new IllegalStateException("pre-auth dispatch scenario failed", error);
        } finally {
            closeQuietly(preAuth);
        }

        String actorId = "actor-sm-d7-" + UUID.randomUUID().toString().replace("-", "");
        ZLinkStreamConnector authenticated = createStreamConnector(options().streamAEndpoint());
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Auth Ok", 7, List.of("auth"));
        try {
            authenticated.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = authenticated
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-D7 auth actor mismatch");

            var push = authenticated.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorEchoRes reply = authenticated
                .request(new Contracts.ActorEchoReq("auth-ok", 7, profile))
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify notify = push.toCompletableFuture().join().payload();

            ensure(actorId.equals(reply.actorId()), "SM-D7 relay actor mismatch");
            ensure("entry:auth-ok".equals(reply.value()), "SM-D7 relay value mismatch");
            ensure(actorId.equals(notify.actorId()), "SM-D7 push actor mismatch");
            ensure("push:auth-ok".equals(notify.value()), "SM-D7 push value mismatch");

            waitForPlayAEvidence(List.of("StreamInbound|play-a|session|ActorAuthReq"));
            System.out.println("scenario SM-D7 passed");
        } catch (Exception error) {
            throw new IllegalStateException("stream auth scenario failed", error);
        } finally {
            closeQuietly(authenticated);
        }

    }
}
