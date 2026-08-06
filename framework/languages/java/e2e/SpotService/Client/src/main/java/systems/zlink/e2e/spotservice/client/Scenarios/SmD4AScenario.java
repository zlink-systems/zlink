package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD4AScenario extends SpotServiceScenarioContext {
    private SmD4AScenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmD4AScenario(context).execute();
    }

    private void execute() {
        String suffix = UUID.randomUUID().toString().replace("-", "");
        String actorId = "actor-sm-d4a-" + suffix;
        String sessionACompanion = "actor-sm-d4a-a-companion-" + suffix;
        String sessionBCompanion = "actor-sm-d4a-b-companion-" + suffix;
        Contracts.ActorProfile profile =
            new Contracts.ActorProfile("SM-D4A", 4, List.of("rebind"));
        ZLinkStreamConnector sessionA = createStreamConnector(options().streamAEndpoint());
        ZLinkStreamConnector sessionB = createStreamConnector(options().streamBEndpoint());
        try {
            sessionA.connect().submit().toCompletableFuture().join();
            sessionB.connect().submit().toCompletableFuture().join();
            bindPair(sessionA, actorId, sessionACompanion, profile);
            Contracts.ActorManagerProbeRes before = findActor(actorId);
            Contracts.ActorPingRes beforeReply = ping(sessionA, actorId, "before-rebind");

            bindPair(sessionB, actorId, sessionBCompanion, profile);
            Contracts.ActorPingRes current = ping(sessionB, actorId, "current-before-stale");
            ensure(current.seen() == beforeReply.seen() + 1,
                "SM-D4A current binding did not preserve Actor state");

            expectFailure(() -> ping(sessionA, actorId, "stale-relay"));
            ping(sessionA, sessionACompanion, "session-a-companion");
            closeQuietly(sessionA);

            Contracts.ActorPingRes after = ping(sessionB, actorId, "current-after-stale");
            ensure(after.seen() == current.seen() + 1,
                "SM-D4A stale relay or disconnect changed the current binding");
            ping(sessionB, sessionBCompanion, "session-b-companion");
            Contracts.ActorManagerProbeRes afterRef = findActor(actorId);
            ensure(before.actor().generation() == afterRef.actor().generation(),
                "SM-D4A rebind changed ObjectGeneration");
            ensure(before.actor().nodeRid().equals(afterRef.actor().nodeRid()),
                "SM-D4A rebind changed Actor owner");
            System.out.println("scenario SM-D4A passed");
        } finally {
            closeQuietly(sessionA);
            closeQuietly(sessionB);
        }
    }

    private void bindPair(
        ZLinkStreamConnector connector,
        String actorId,
        String companionId,
        Contracts.ActorProfile profile) {
        Contracts.MultiBindRes result = connector
            .request(new Contracts.MultiBindReq(actorId, companionId, profile))
            .submit(Contracts.MultiBindRes.class).toCompletableFuture().join();
        ensure(result.boundCount() == 2, "SM-D4A expected two current bindings");
    }

    private Contracts.ActorPingRes ping(ZLinkStreamConnector connector, String actorId, String value) {
        return connector.request(new Contracts.ActorPingReq(value))
            .metadata("actor-id", actorId)
            .submit(Contracts.ActorPingRes.class).toCompletableFuture().join();
    }

    private Contracts.ActorManagerProbeRes findActor(String actorId) {
        return postJson(options().httpAEndpoint(), "/actor/manager-probe",
            new Contracts.ActorManagerProbeReq("find", actorId),
            Contracts.ActorManagerProbeRes.class);
    }
}
