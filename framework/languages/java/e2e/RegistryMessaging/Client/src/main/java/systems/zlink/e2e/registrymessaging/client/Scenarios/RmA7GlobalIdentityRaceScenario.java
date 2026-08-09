package Scenarios;

import systems.zlink.e2e.registrymessaging.client.Scenarios;
import systems.zlink.e2e.registrymessaging.client.Support;
import java.util.Arrays;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import systems.zlink.e2e.registrymessaging.client.Support.ClientOptions;
import systems.zlink.e2e.registrymessaging.client.Support.DynamicClusterLauncher;
import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmA7GlobalIdentityRaceScenario {
    private RmA7GlobalIdentityRaceScenario() {
    }

    public static void run(ClientOptions options) {
        try (DynamicClusterLauncher cluster = DynamicClusterLauncher.start(options)) {
            DynamicClusterLauncher.DynamicProvider profile =
                cluster.startObjectProvider("profile-owner", "profile-owner", "profile-mesh");
            DynamicClusterLauncher.DynamicWorkflow workflow =
                cluster.startWorkflow("workflow-owner", "workflow-owner", "workflow-mesh");
            try (ZLinkHttpClient profileHttp = ZLinkHttpClient.create(profile.httpUrl()).build();
                 ZLinkHttpClient workflowHttp = ZLinkHttpClient.create(workflow.httpUrl()).build()) {
                String suffix = Long.toUnsignedString(System.nanoTime());
                Contracts.IdentityCreateReq profileRequest = new Contracts.IdentityCreateReq(
                    "rm-a7-actor-" + suffix,
                    "rm-a7-spot-" + suffix,
                    "profile-mesh",
                    "rm-a7-profile-" + suffix);
                Contracts.IdentityCreateReq workflowRequest = new Contracts.IdentityCreateReq(
                    profileRequest.actorId(),
                    profileRequest.spotId(),
                    "workflow-mesh",
                    "rm-a7-workflow-" + suffix);

                CompletableFuture<Contracts.IdentityCreateRes> profileCreate = CompletableFuture
                    .supplyAsync(() -> create(profileHttp, profileRequest));
                CompletableFuture<Contracts.IdentityCreateRes> workflowCreate = CompletableFuture
                    .supplyAsync(() -> create(workflowHttp, workflowRequest));
                Contracts.IdentityCreateRes first = profileCreate.join();
                Contracts.IdentityCreateRes second = workflowCreate.join();

                ScenarioAssert.that(
                    Set.of(first.actorState(), second.actorState()).equals(Set.of("CREATED", "EXISTING")),
                    "RM-A7 actor creation did not have exactly one winner: "
                        + first.actorState() + "," + second.actorState());
                ScenarioAssert.that(
                    Set.of(first.spotState(), second.spotState()).equals(Set.of("CREATED", "EXISTING")),
                    "RM-A7 Spot creation did not have exactly one winner: "
                        + first.spotState() + "," + second.spotState());
                ScenarioAssert.that(first.actor() != null && first.spot() != null,
                    "RM-A7 winner did not return both public refs");
                ScenarioAssert.that(first.actor().equals(second.actor())
                        && first.actor().equals(first.actorFound())
                        && first.actor().equals(second.actorFound()),
                    "RM-A7 returned duplicate or divergent Actor refs");
                ScenarioAssert.that(first.spot().equals(second.spot())
                        && first.spot().equals(first.spotFound())
                        && first.spot().equals(second.spotFound()),
                    "RM-A7 returned duplicate or divergent Spot refs");

                ZLinkHttpClient actorHttp = first.actor().meshName().equals("profile-mesh")
                    ? profileHttp : workflowHttp;
                ZLinkHttpClient spotHttp = first.spot().meshName().equals("profile-mesh")
                    ? profileHttp : workflowHttp;
                Contracts.IdentityActorPingRes actorReply = pingActor(actorHttp,
                    new Contracts.IdentityActorDirectReq(
                        profileRequest.actorId(), "rm-a7-actor-ping-" + suffix));
                Contracts.IdentitySpotPingRes spotReply = pingSpot(spotHttp,
                    new Contracts.IdentitySpotDirectReq(
                        profileRequest.spotId(), first.spot().meshName(), "rm-a7-spot-ping-" + suffix));
                ScenarioAssert.that(actorReply.actorId().equals(profileRequest.actorId())
                        && actorReply.objectGeneration()
                            == first.actor().objectGeneration(),
                    "RM-A7 Actor direct request did not use one current identity");
                ScenarioAssert.that(spotReply.spotId().equals(profileRequest.spotId())
                        && spotReply.objectGeneration()
                            == first.spot().objectGeneration(),
                    "RM-A7 Spot direct request did not use one current identity");
                ScenarioAssert.that(actorReply.sequence() == 1 && spotReply.sequence() == 1,
                    "RM-A7 direct request did not produce one handler delivery per identity");

                String[] profileEvidence = ScenarioAssert.evidence(profileHttp);
                String[] workflowEvidence = ScenarioAssert.evidence(workflowHttp);
                ScenarioAssert.that(Arrays.stream(profileEvidence)
                        .anyMatch(line -> line.startsWith("IdentityCreate|")),
                    "RM-A7 profile role did not expose identity evidence");
                ScenarioAssert.that(Arrays.stream(workflowEvidence)
                        .anyMatch(line -> line.startsWith("IdentityCreate|")),
                    "RM-A7 workflow role did not expose identity evidence");
            }
        }
        System.out.println("scenario RM-A7 passed");
    }

    private static Contracts.IdentityCreateRes create(
        ZLinkHttpClient client,
        Contracts.IdentityCreateReq request) {
        return client.post("/identity/create")
            .body(request)
            .submit(Contracts.IdentityCreateRes.class)
            .toCompletableFuture().join().body();
    }

    private static Contracts.IdentityActorPingRes pingActor(
        ZLinkHttpClient client,
        Contracts.IdentityActorDirectReq request) {
        return client.post("/identity/ping-actor")
            .body(request)
            .submit(Contracts.IdentityActorPingRes.class)
            .toCompletableFuture().join().body();
    }

    private static Contracts.IdentitySpotPingRes pingSpot(
        ZLinkHttpClient client,
        Contracts.IdentitySpotDirectReq request) {
        return client.post("/identity/ping-spot")
            .body(request)
            .submit(Contracts.IdentitySpotPingRes.class)
            .toCompletableFuture().join().body();
    }
}
