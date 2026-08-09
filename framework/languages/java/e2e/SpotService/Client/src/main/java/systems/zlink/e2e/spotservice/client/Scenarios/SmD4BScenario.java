package Scenarios;

import systems.zlink.e2e.spotservice.client.Scenarios;
import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class SmD4BScenario extends SpotServiceScenarioContext {
    private SmD4BScenario(SpotServiceScenarioContext context) { super(context); }

    public static void run(SpotServiceScenarioContext context) {
        new SmD4BScenario(context).execute();
    }

    private void execute() {
        String suffix = UUID.randomUUID().toString().replace("-", "");
        String actorId = "actor-sm-d4b-" + suffix;
        ZLinkStreamConnector session = createStreamConnector(options().streamAEndpoint());
        setPlacementWeight(options().httpAEndpoint(), 100);
        setPlacementWeight(options().httpBEndpoint(), 0);
        try {
            session.connect().submit().toCompletableFuture().join();
            Contracts.ActorManagerProbeRes before = postJson(options().httpAEndpoint(),
                "/actor/manager-probe",
                new Contracts.ActorManagerProbeReq("create", actorId),
                Contracts.ActorManagerProbeRes.class);
            Contracts.MultiBindRes bound = session.request(new Contracts.MultiBindReq(
                    actorId,
                    "actor-sm-d4b-companion-" + suffix,
                    new Contracts.ActorProfile("SM-D4B", 4, List.of("follow"))))
                .submit(Contracts.MultiBindRes.class).toCompletableFuture().join();
            ensure(bound.boundCount() == 2, "SM-D4B did not create the stored session bindings");
            Contracts.ActorPingRes first = session.request(new Contracts.ActorPingReq("before-relocation"))
                .metadata("actor-id", actorId)
                .submit(Contracts.ActorPingRes.class).toCompletableFuture().join();

            setPlacementWeight(options().httpAEndpoint(), 0);
            setPlacementWeight(options().httpBEndpoint(), 100);
            Contracts.RelocationRes relocation = relocate(options().httpAEndpoint());
            ensure(relocation.outcome().equals("RELOCATED"),
                "SM-D4B relocation did not complete: " + relocation.outcome());
            Contracts.ActorManagerProbeRes after = eventually(() -> postJson(options().httpBEndpoint(),
                "/actor/manager-probe",
                new Contracts.ActorManagerProbeReq("find", actorId),
                Contracts.ActorManagerProbeRes.class));
            ensure(!before.actor().nodeRid().equals(after.actor().nodeRid()),
                "SM-D4B did not relocate the bound Actor");
            Contracts.ActorPingRes followed = session.request(new Contracts.ActorPingReq("after-relocation"))
                .metadata("actor-id", actorId)
                .submit(Contracts.ActorPingRes.class).toCompletableFuture().join();
            ensure(followed.seen() == first.seen() + 1,
                "SM-D4B stored binding did not follow the relocated Actor");
            System.out.println("scenario SM-D4B passed");
        } finally {
            setPlacementWeight(options().httpAEndpoint(), 100);
            setPlacementWeight(options().httpBEndpoint(), 100);
            closeQuietly(session);
        }
    }
}
