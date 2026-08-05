package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamMessage;

public final class AtdD4SessionRelayActorAwaitScenario {
    private AtdD4SessionRelayActorAwaitScenario() {
    }

    public static void run(
        ZLinkStreamConnector connector,
        String actorId,
        ZLinkStreamConnector unbound) throws Exception {
        String requestId = "ATD-D4-" + UUID.randomUUID().toString().replace("-", "");
        CompletionStage<Void> unboundPush = unbound
            .expectNone(Contracts.ActorPushNotify.class)
            .within(Duration.ofMillis(400))
            .submit();
        CompletionStage<ZLinkStreamMessage<Contracts.ActorPushNotify>> push = connector
            .waitFor(Contracts.ActorPushNotify.class)
            .timeout(ClientStreamSupport.REQUEST_TIMEOUT)
            .submit(Contracts.ActorPushNotify.class);
        Contracts.ActorRes reply = ClientStreamSupport.await(
            connector.request(new Contracts.ActorPushAwaitReq(requestId, 350, "bound-session-push"))
                .metadata("actor-id", actorId)
                .timeout(ClientStreamSupport.REQUEST_TIMEOUT),
            Contracts.ActorRes.class);
        Contracts.ActorPushNotify notify = push.toCompletableFuture().join().payload();
        ScenarioAssert.that("ATD-D4".equals(reply.scenarioId()), "ATD-D4 reply scenario mismatch");
        ScenarioAssert.that(actorId.equals(reply.actorId()), "ATD-D4 reply actor mismatch");
        ScenarioAssert.that("actor-push-await-completed".equals(reply.marker()), "ATD-D4 reply marker mismatch");
        ScenarioAssert.that(actorId.equals(notify.actorId()), "ATD-D4 push actor mismatch");
        ScenarioAssert.that(requestId.equals(notify.requestId()), "ATD-D4 push request mismatch");
        ScenarioAssert.that("bound-session-push".equals(notify.value()), "ATD-D4 push value mismatch");
        unboundPush.toCompletableFuture().join();
        Contracts.EvidenceRes evidence = ClientStreamSupport.evidence(connector, requestId);
        ScenarioAssert.containsMarkersInOrder(evidence.markers(),
            "actor-push-await-started",
            "actor-push-await-released",
            "actor-push-await-resumed",
            "actor-push-await-completed");
        System.out.println("scenario ATD-D4 passed");
    }
}
