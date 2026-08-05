package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;

public final class SmD14Scenario extends SpotServiceScenarioContext {
    private SmD14Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD14Scenario(context).execute();
    }

    private void execute() {
        String endpoint = options().tlsStreamAEndpoint();
        ZLinkStreamConnector strict = createStreamConnector(
            endpoint,
            ZLinkStreamDispatchMode.IMMEDIATE,
            Integer.MAX_VALUE,
            false);
        boolean strictTlsRejected = false;
        try {
            strict.connect().submit().toCompletableFuture().join();
        } catch (Exception error) {
            strictTlsRejected = true;
        } finally {
            closeQuietly(strict);
        }
        ensure(strictTlsRejected, "SM-D14 expected strict TLS validation to reject self-signed certificate");

        String actorId = "actor-sm-d14-tls";
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Stream TLS", 14, List.of("tls"));
        ZLinkStreamConnector tls = createStreamConnector(
            endpoint,
            ZLinkStreamDispatchMode.IMMEDIATE,
            Integer.MAX_VALUE,
            true);
        try {
            tls.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = tls
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-D14 TLS auth actor mismatch");
            ensure("play-a".equals(auth.nodeRid()), "SM-D14 TLS auth node mismatch");

            var pushed = tls.waitFor(Contracts.ActorPushNotify.class)
                .submit(Contracts.ActorPushNotify.class);
            Contracts.ActorEchoRes reply = tls
                .request(new Contracts.ActorEchoReq("tls-push", 14, profile))
                .metadata("actor-id", actorId)
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            Contracts.ActorPushNotify notify = pushed.toCompletableFuture().join().payload();
            ensure(actorId.equals(reply.actorId()), "SM-D14 TLS actor reply mismatch");
            ensure("play-a".equals(reply.nodeRid()), "SM-D14 TLS actor node mismatch");
            ensure(actorId.equals(notify.actorId()), "SM-D14 TLS push actor mismatch");
            ensure("push:tls-push".equals(notify.value()), "SM-D14 TLS push payload mismatch");

            System.out.println("scenario SM-D14 passed");
        } catch (Exception error) {
            throw new IllegalStateException("stream TLS scenario failed", error);
        } finally {
            closeQuietly(tls);
        }

    }
}
