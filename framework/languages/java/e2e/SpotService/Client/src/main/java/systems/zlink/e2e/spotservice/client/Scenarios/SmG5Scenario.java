package Scenarios;
import systems.zlink.e2e.spotservice.client.Scenarios;
import java.time.Duration;

import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmG5Scenario extends SpotServiceScenarioContext {
    private SmG5Scenario(SpotServiceScenarioContext context) { super(context); }

    public static void runWeight(SpotServiceScenarioContext context) { new SmG5Scenario(context).weight(); }
    public static void runCapacity(SpotServiceScenarioContext context) { new SmG5Scenario(context).capacity(); }

    private void weight() {
        String suffix = UUID.randomUUID().toString().replace("-", "");
        setPlacementWeight(options().httpAEndpoint(), 100);
        setPlacementWeight(options().httpBEndpoint(), 0);
        Contracts.ActorManagerProbeRes existing = postJson(options().httpAEndpoint(), "/actor/manager-probe",
            new Contracts.ActorManagerProbeReq("create", "actor-sm-g5-existing-" + suffix),
            Contracts.ActorManagerProbeRes.class);
        setPlacementWeight(options().httpAEndpoint(), 100);
        setPlacementWeight(options().httpBEndpoint(), 300);
        int actorPlayA = 0;
        int actorPlayB = 0;
        for (int batch = 0; batch < 8; batch++) {
            Contracts.PlacementBatchRes result = postJson(options().httpAEndpoint(), "/placement/batch",
                new Contracts.PlacementBatchReq(suffix + "-" + batch, 100),
                Contracts.PlacementBatchRes.class, Duration.ofMinutes(2));
            actorPlayA += result.actorPlayA();
            actorPlayB += result.actorPlayB();
            ensure(result.spotCount() == 0, "SM-G5A created a Spot during Actor placement");
        }
        ensure(actorPlayA + actorPlayB == 800
                && actorPlayB * 100 >= 65 * 800 && actorPlayB * 100 <= 85 * 800,
            "SM-G5A placement distribution was outside the contract range");
        Contracts.ActorManagerProbeRes after = postJson(options().httpAEndpoint(), "/actor/manager-probe",
            new Contracts.ActorManagerProbeReq("find", existing.actor().actorId()),
            Contracts.ActorManagerProbeRes.class);
        ensure(existing.actor().generation() == after.actor().generation()
                && existing.actor().nodeRid().equals(after.actor().nodeRid()),
            "SM-G5A changed the owner of an existing Actor");
        setPlacementWeight(options().httpAEndpoint(), 100);
        setPlacementWeight(options().httpBEndpoint(), 100);
        System.out.println("scenario SM-G5A passed");
    }

    private void capacity() {
        setPlacementWeight(options().httpAEndpoint(), 10000);
        setPlacementWeight(options().httpBEndpoint(), 10000);
        Contracts.PlacementProbeRes valid = postJson(options().httpAEndpoint(), "/placement/probe",
            new Contracts.PlacementWeightReq(10000), Contracts.PlacementProbeRes.class);
        Contracts.PlacementProbeRes invalid = postJson(options().httpAEndpoint(), "/placement/probe",
            new Contracts.PlacementWeightReq(-1), Contracts.PlacementProbeRes.class);
        ensure(valid.accepted() && invalid.accepted() == false && invalid.current() == 10000,
            "SM-G5B placement validation did not reject an invalid weight");
        Contracts.CapacityPlacementRes result = postJson(options().httpAEndpoint(), "/placement/capacity?suffix="
                + UUID.randomUUID().toString().replace("-", ""), new Contracts.OperationAccepted(false),
            Contracts.CapacityPlacementRes.class, EVENTUAL_TIMEOUT);
        ensure(result.firstNodeRid().startsWith("play-b") && result.secondNodeRid().startsWith("play-a"),
            "SM-G5B did not select the eligible node after the high-weight node reached capacity");
        setPlacementWeight(options().httpAEndpoint(), 100);
        setPlacementWeight(options().httpBEndpoint(), 100);
        System.out.println("scenario SM-G5B passed");
    }
}
