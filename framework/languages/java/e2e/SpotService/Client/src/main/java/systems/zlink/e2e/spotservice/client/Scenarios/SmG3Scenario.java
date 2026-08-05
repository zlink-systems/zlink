package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmG3Scenario extends SpotServiceScenarioContext {
    private SmG3Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmG3Scenario(context).execute();
    }

    private void execute() {
        int actorCount = 2;
        String key = UUID.randomUUID().toString().replace("-", "");
        String spotRid = "spot-sm-g3-" + key;
        List<String> actorIds = new ArrayList<>();
        List<ZLinkStreamConnector> connectors = new ArrayList<>();
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Race", 3, List.of("join", "leave"));
        try {
            postJson(
                options().httpAEndpoint(),
                "/spot/create",
                new Contracts.CreateSpotReq(spotRid),
                Contracts.CreateSpotRes.class);

            for (int index = 0; index < actorCount; index++) {
                String actorId = "actor-sm-g3-" + key + "-" + index;
                ZLinkStreamConnector connector = createStreamConnector(options().streamAEndpoint());
                connector.connect().submit().toCompletableFuture().join();
                Contracts.ActorAuthRes auth = connector
                    .request(new Contracts.ActorAuthReq(actorId, profile))
                    .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
                ensure(actorId.equals(auth.actorId()), "SM-G3 auth actor mismatch");
                Contracts.ActorJoinRes joined = connector
                    .request(new Contracts.ActorJoinReq(spotRid, profile, profile.tags()))
                    .metadata("actor-id", actorId)
                    .submit(Contracts.ActorJoinRes.class).toCompletableFuture().join();
                ensure(actorId.equals(joined.actorId()), "SM-G3 join actor mismatch");
                ensure("play-a".equals(joined.nodeRid()), "SM-G3 join node mismatch");
                actorIds.add(actorId);
                connectors.add(connector);
            }

            List<CompletableFuture<Void>> tasks = new ArrayList<>();
            for (int index = 0; index < actorIds.size(); index++) {
                String actorId = actorIds.get(index);
                ZLinkStreamConnector connector = connectors.get(index);
                tasks.add(CompletableFuture.runAsync(() -> {
                    try {
                        Contracts.ActorEchoRes ping = connector
                            .request(new Contracts.ActorEchoReq(actorId, 3, profile))
                            .metadata("actor-id", actorId)
                            .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
                        ensure(actorId.equals(ping.actorId()), "SM-G3 actor request target mismatch");
                        ensure("play-a".equals(ping.nodeRid()), "SM-G3 actor request node mismatch");
                        connector
                            .send(new Contracts.LeaveActorReq(actorId))
                            .metadata("actor-id", actorId)
                            .submit();
                    } catch (Exception error) {
                        throw new RuntimeException(error);
                    }
                }));
            }
            CompletableFuture.allOf(tasks.toArray(CompletableFuture[]::new)).join();

            List<String> expected = actorIds.stream()
                .flatMap(actorId -> List.of(
                    "ActorUserJoined|play-a|" + spotRid + "|" + actorId,
                    "ActorUserLeft|play-a|" + spotRid + "|" + actorId).stream())
                .toList();
            Contracts.EvidenceSnapshot evidence = waitForPlayAEvidence(expected);
            for (String actorId : actorIds) {
                long joinedCount = countActorEvidence(evidence, "ActorUserJoined", spotRid, actorId);
                long leftCount = countActorEvidence(evidence, "ActorUserLeft", spotRid, actorId);
                ensure(joinedCount == 1,
                    "SM-G3 join evidence count mismatch for " + actorId
                        + ": count=" + joinedCount
                        + " matches=" + matchingActorEvidence(evidence, "ActorUserJoined", spotRid, actorId));
                ensure(leftCount == 1,
                    "SM-G3 leave evidence count mismatch for " + actorId
                        + ": count=" + leftCount
                        + " matches=" + matchingActorEvidence(evidence, "ActorUserLeft", spotRid, actorId));
            }
            System.out.println("scenario SM-G3 passed");
        } catch (Exception error) {
            throw new IllegalStateException("join/leave race scenario failed", error);
        } finally {
            for (ZLinkStreamConnector connector : connectors) {
                closeQuietly(connector);
            }
        }

    }
}
