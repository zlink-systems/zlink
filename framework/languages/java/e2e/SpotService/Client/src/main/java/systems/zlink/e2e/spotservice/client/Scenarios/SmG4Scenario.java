package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamMessage;

public final class SmG4Scenario extends SpotServiceScenarioContext {
    private SmG4Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmG4Scenario(context).execute();
    }

    private void execute() {
        int sessionCount = 8;
        List<ZLinkStreamConnector> connectors = new ArrayList<>();
        List<String> actorIds = new ArrayList<>();
        List<String> values = new ArrayList<>();
        List<CompletionStage<ZLinkStreamMessage<Contracts.ActorPushNotify>>> pushes = new ArrayList<>();
        try {
            for (int index = 0; index < sessionCount; index++) {
                String actorId = "actor-sm-g4-" + index + "-" + UUID.randomUUID().toString().replace("-", "");
                Contracts.ActorProfile profile =
                    new Contracts.ActorProfile("Bound Load " + index, 14, List.of("load", "session-" + index));
                ZLinkStreamConnector connector = createStreamConnector(options().streamAEndpoint());
                connectors.add(connector);
                actorIds.add(actorId);
                values.add("push-" + index);
                connector.connect().submit().toCompletableFuture().join();
                Contracts.ActorAuthRes auth = connector
                    .request(new Contracts.ActorAuthReq(actorId, profile))
                    .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
                ensure(actorId.equals(auth.actorId()), "SM-G4 auth actor mismatch");
            }

            for (ZLinkStreamConnector connector : connectors) {
                pushes.add(connector.waitFor(Contracts.ActorPushNotify.class)
                    .submit(Contracts.ActorPushNotify.class));
            }

            List<CompletableFuture<Contracts.ActorEchoRes>> replies = new ArrayList<>();
            for (int index = 0; index < connectors.size(); index++) {
                ZLinkStreamConnector connector = connectors.get(index);
                String value = values.get(index);
                Contracts.ActorProfile profile =
                    new Contracts.ActorProfile("Bound Load Request " + index, 14, List.of("load"));
                replies.add(CompletableFuture.supplyAsync(() -> {
                    try {
                        return connector
                            .request(new Contracts.ActorEchoReq(value, 14, profile))
                            .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
                    } catch (Exception error) {
                        throw new RuntimeException(error);
                    }
                }));
            }

            for (int index = 0; index < connectors.size(); index++) {
                String actorId = actorIds.get(index);
                String value = values.get(index);
                Contracts.ActorEchoRes reply = replies.get(index).join();
                Contracts.ActorPushNotify notify = pushes.get(index).toCompletableFuture().join().payload();
                ensure(actorId.equals(reply.actorId()), "SM-G4 push reply actor mismatch");
                ensure(("entry:" + value).equals(reply.value()), "SM-G4 push reply value mismatch");
                ensure(actorId.equals(notify.actorId()), "SM-G4 push notify actor mismatch");
                ensure(("push:" + value).equals(notify.value()), "SM-G4 push notify value mismatch");
            }

            System.out.println("scenario SM-G4 passed");
        } catch (Exception error) {
            throw new IllegalStateException("bound session push load scenario failed", error);
        } finally {
            for (ZLinkStreamConnector connector : connectors) {
                closeQuietly(connector);
            }
        }

    }
}
