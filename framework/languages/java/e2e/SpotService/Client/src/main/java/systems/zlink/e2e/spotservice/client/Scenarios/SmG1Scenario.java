package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmG1Scenario extends SpotServiceScenarioContext {
    private SmG1Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmG1Scenario(context).execute();
    }

    private void execute() {
        String actorId = "actor-sm-g1-" + UUID.randomUUID().toString().replace("-", "");
        Contracts.ActorProfile profile = new Contracts.ActorProfile("Crash Recovery", 1, List.of("sm-g1"));
        ZLinkStreamConnector crashedConnector = createStreamConnector(options().streamAEndpoint());
        try {
            crashedConnector.connect().submit().toCompletableFuture().join();
            authenticateJoinAndEcho(crashedConnector, actorId, profile, "before-crash", 1);
            signalFile(options().readyFile());
            waitForSignalFile(options().crashedFile());

            expectFailure(() -> {
                try {
                    crashedConnector
                        .request(new Contracts.ActorEchoReq("during-crash", 2, profile))
                        .metadata("actor-id", actorId)
                        .timeout(Duration.ofSeconds(2))
                        .submit(Contracts.ActorEchoRes.class).toCompletableFuture().join();
                } catch (Exception error) {
                    throw new RuntimeException(error);
                }
            });

            Contracts.StateRes survivor = eventually(() -> requestState("room-b", "sm-g1-survivor", REQUEST_TIMEOUT));
            ensure("play-b".equals(survivor.nodeRid()), "SM-G1 play-b survivor request node mismatch");
            signalFile(options().failedFile());
            waitForSignalFile(options().restartedFile());
        } catch (Exception error) {
            throw new IllegalStateException("play crash recovery scenario failed before restart", error);
        } finally {
            closeQuietly(crashedConnector);
        }

        ZLinkStreamConnector recoveredConnector = createStreamConnector(options().streamAEndpoint());
        try {
            recoveredConnector.connect().submit().toCompletableFuture().join();
            authenticateJoinAndEcho(recoveredConnector, actorId, profile, "after-restart", 3);
            signalFile(options().secondCrashReadyFile());
            waitForSignalFile(options().secondCrashedFile());
            Contracts.StateRes survivor = eventually(() ->
                requestState("room-b", "sm-g1-second-crash-survivor", REQUEST_TIMEOUT));
            ensure("play-b".equals(survivor.nodeRid()),
                "SM-G1 second crash lost the surviving play-b reply");
        } catch (Exception error) {
            throw new IllegalStateException(
                "play crash recovery scenario failed after restart or second crash", error);
        } finally {
            closeQuietly(recoveredConnector);
        }

        System.out.println("scenario SM-G1 passed");

    }
}
