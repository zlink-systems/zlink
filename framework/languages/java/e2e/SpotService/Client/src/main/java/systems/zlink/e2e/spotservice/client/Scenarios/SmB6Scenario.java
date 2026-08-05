package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmB6Scenario extends SpotServiceScenarioContext {
    private SmB6Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmB6Scenario(context).execute();
    }

    private void execute() {
        try {
            String suffix = UUID.randomUUID().toString().replace("-", "");
            String leaveActorId = "actor-sm-b6-left-" + suffix;
            String disconnectActorId = "actor-sm-b6-disconnected-" + suffix;

            try {
                ZLinkStreamConnector leaveClient = createStreamConnector(options().streamAEndpoint());
                Contracts.ActorProfile profile = new Contracts.ActorProfile("Leave", 6, List.of("leave"));
                try {
                    leaveClient.connect().submit().toCompletableFuture().join();
                    Contracts.ActorAuthRes auth = leaveClient
                        .request(new Contracts.ActorAuthReq(leaveActorId, profile))
                        .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
                    ensure(leaveActorId.equals(auth.actorId()), "SM-B6 leave auth actor mismatch");
                    Contracts.ActorJoinRes joined = leaveClient
                        .request(new Contracts.ActorJoinReq("room-a", profile, profile.tags()))
                        .metadata("actor-id", leaveActorId)
                        .submit(Contracts.ActorJoinRes.class).toCompletableFuture().join();
                    ensure(leaveActorId.equals(joined.actorId()), "SM-B6 leave join actor mismatch");
                    leaveClient
                        .send(new Contracts.LeaveActorReq(leaveActorId))
                        .metadata("actor-id", leaveActorId)
                        .submit();
                } finally {
                    closeQuietly(leaveClient);
                }
            } catch (Exception error) {
                throw new IllegalStateException("SM-B6 leave phase failed", error);
            }

            Contracts.EvidenceSnapshot leaveEvidence = waitForPlayAEvidence(
                List.of("ActorUserLeft|play-a|room-a|" + leaveActorId));
            ensure(leaveEvidence.entries().stream().noneMatch(entry ->
                    "ActorUserDisconnected".equals(entry.marker()) && leaveActorId.equals(entry.value())),
                "SM-B6 explicit leave emitted disconnect evidence");

            try {
                ZLinkStreamConnector disconnectClient =
                    createStreamConnector(options().streamAEndpoint());
                try {
                    Contracts.ActorProfile disconnectProfile =
                        new Contracts.ActorProfile("Disconnect", 6, List.of("disconnect"));
                    disconnectClient.connect().submit().toCompletableFuture().join();
                    Contracts.ActorAuthRes auth = disconnectClient
                        .request(new Contracts.ActorAuthReq(disconnectActorId, disconnectProfile))
                        .submit(Contracts.ActorAuthRes.class).toCompletableFuture().join();
                    ensure(disconnectActorId.equals(auth.actorId()), "SM-B6 disconnect auth actor mismatch");
                    Contracts.ActorJoinRes joined = disconnectClient
                        .request(new Contracts.ActorJoinReq("room-a", disconnectProfile, disconnectProfile.tags()))
                        .metadata("actor-id", disconnectActorId)
                        .submit(Contracts.ActorJoinRes.class).toCompletableFuture().join();
                    ensure(disconnectActorId.equals(joined.actorId()), "SM-B6 disconnect join actor mismatch");
                } finally {
                    closeQuietly(disconnectClient);
                }
            } catch (Exception error) {
                throw new IllegalStateException("SM-B6 disconnect phase failed", error);
            }

            Contracts.EvidenceSnapshot disconnectEvidence = waitForPlayAEvidence(
                List.of("ActorUserDisconnected|play-a|room-a|" + disconnectActorId));
            ensure(disconnectEvidence.entries().stream().noneMatch(entry ->
                    "ActorUserLeft".equals(entry.marker()) && disconnectActorId.equals(entry.value())),
                "SM-B6 disconnect emitted leave evidence");

            System.out.println("scenario SM-B6 passed");
        } catch (Exception error) {
            throw new IllegalStateException("actor leave/disconnect scenario failed", error);
        }

    }
}
