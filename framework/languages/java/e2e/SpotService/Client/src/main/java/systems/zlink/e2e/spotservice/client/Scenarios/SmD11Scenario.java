package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD11Scenario extends SpotServiceScenarioContext {
    private SmD11Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD11Scenario(context).execute();
    }

    private void execute() {
        String actorId = "actor-sm-d11-" + UUID.randomUUID().toString().replace("-", "");
        ZLinkStreamConnector connector = createStreamConnector(options().streamAEndpoint());
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Mixed", 11, List.of("stream", "channel"));
        try {
            connector.connect().submit().toCompletableFuture().join();
            Contracts.ActorAuthRes auth = connector
                .request(new Contracts.ActorAuthReq(actorId, profile))
                .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
            ensure(actorId.equals(auth.actorId()), "SM-D11 auth actor mismatch");

            Contracts.ActorEchoRes streamReply = connector
                .request(new Contracts.ActorEchoReq("mixed-stream", 11, profile))
                .metadata("actor-id", actorId)
                .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
            ensure("entry:mixed-stream".equals(streamReply.value()), "SM-D11 stream actor reply mismatch");

            Contracts.RouteRes channelReply = requestRoute("play-a", "mixed-channel");
            ensure("route:mixed-channel".equals(channelReply.value()), "SM-D11 route-channel reply mismatch");
            ensure("play-a".equals(channelReply.nodeRid()), "SM-D11 route-channel node mismatch");

            waitForPlayAEvidence(List.of(
                "StreamInbound|play-a|session|ActorEchoReq",
                "RouteReq|play-a|client-route-mesh|mixed-channel"));
            System.out.println("scenario SM-D11 passed");
        } catch (Exception error) {
            throw new IllegalStateException("mixed stream/channel scenario failed", error);
        } finally {
            closeQuietly(connector);
        }

    }
}
