package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.kotlin.automaticturn.Contracts;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamCloseReason;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamDisconnected;
import systems.zlink.stream.connector.ZLinkStreamMessage;

public final class JvmSessionReplacementScenario {
    private JvmSessionReplacementScenario() {
    }

    public static void run(
        ZLinkStreamConnector retiredSession,
        ZLinkStreamConnector currentSession) {
        String requestId = "jvm-session-" + System.nanoTime();
        String actorA = requestId + "-actor-a";
        String actorB = requestId + "-actor-b";
        Contracts.BindActorsRes bind = ClientStreamSupport.bindActors(
            retiredSession,
            "room-a",
            actorA,
            actorB);
        ScenarioAssert.that(actorA.equals(bind.actorA()),
            "JVM-SESSION-001 actor A bind mismatch");
        ScenarioAssert.that(actorB.equals(bind.actorB()),
            "JVM-SESSION-001 actor B bind mismatch");

        CompletionStage<ZLinkStreamMessage<Contracts.ActorBindingReplacedNotice>> notice =
            retiredSession
                .waitFor(Contracts.ActorBindingReplacedNotice.class)
                .timeout(Duration.ofSeconds(5))
                .submit(Contracts.ActorBindingReplacedNotice.class);
        CompletableFuture<ZLinkStreamDisconnected> disconnected =
            new CompletableFuture<>();
        AutoCloseable disconnectSubscription = currentDisconnect(
            retiredSession,
            disconnected);
        try {
            Contracts.ActorAuthRes authenticated = ClientStreamSupport.await(
                currentSession.request(new Contracts.ActorAuthReq(actorB)),
                Contracts.ActorAuthRes.class);
            ScenarioAssert.that(actorB.equals(authenticated.actorId()),
                "JVM-SESSION-001 current session bind mismatch");

            Contracts.ActorBindingReplacedNotice callback =
                notice.toCompletableFuture().join().payload();
            ScenarioAssert.that(actorB.equals(callback.actorId()),
                "JVM-SESSION-001 callback actor mismatch");
            long callbackTerminalNanos = System.nanoTime();

            Contracts.ActorRes fast = ClientStreamSupport.await(
                currentSession.request(new Contracts.ActorFastReq(
                    requestId,
                    "replacement-progress"))
                    .metadata("actor-id", actorB),
                Contracts.ActorRes.class);
            ScenarioAssert.that(actorB.equals(fast.actorId()),
                "JVM-SESSION-001 other session lane did not progress");

            ZLinkStreamDisconnected close = disconnected
                .orTimeout(5, TimeUnit.SECONDS)
                .join();
            long closeDelayMillis = TimeUnit.NANOSECONDS.toMillis(
                System.nanoTime() - callbackTerminalNanos);
            ScenarioAssert.that(close.closeReason() == ZLinkStreamCloseReason.SERVER_DRAIN,
                "JVM-SESSION-001 retired session close reason mismatch: "
                    + close.closeReason());
            ScenarioAssert.that(closeDelayMillis >= 80 && closeDelayMillis < 2_000,
                "JVM-SESSION-001 retired session close was not timer bounded: "
                    + closeDelayMillis + "ms");
        } finally {
            try {
                disconnectSubscription.close();
            } catch (Exception ignored) {
            }
        }
        System.out.println("scenario JVM-SESSION-001 passed");
    }

    private static AutoCloseable currentDisconnect(
        ZLinkStreamConnector connector,
        CompletableFuture<ZLinkStreamDisconnected> disconnected) {
        return connector.onDisconnected(event -> {
            disconnected.complete(event);
            return CompletableFuture.completedFuture(null);
        });
    }
}
