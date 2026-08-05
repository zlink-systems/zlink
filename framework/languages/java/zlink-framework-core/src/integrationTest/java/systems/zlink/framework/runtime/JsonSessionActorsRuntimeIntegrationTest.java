package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.streams.ZLinkSessionActor;

final class JsonSessionActorsRuntimeIntegrationTest {
    @Test
    void sessionGateway_relaysJsonActorSendWithDefaultPacketName() throws Exception {
        SessionActorsRuntimeIntegrationTest.actorRelayRequests.clear();
        Zlink.version();
        String actorId =
            SessionActorsRuntimeIntegrationTest.uniqueActorId("json-player");
        try (ZLinkFrameworkRuntime runtime = startLocalJsonRuntime()) {
            ZLinkActor actor = ((ZLinkActorRuntime) runtime.actorManager())
                .getOrCreateManagedActor(actorId, "player")
                .toCompletableFuture()
                .join();
            ZLinkSessionActor bound = runtime.sessionActors(
                    "local-json",
                    RoutingId.from("json-session"))
                .bind(actor)
                .toCompletableFuture()
                .join();

            SessionActorsRuntimeIntegrationTest.relayWithHeader(
                bound,
                "JsonRelaySend",
                ZLinkMessage.of(new JsonRelaySend("json-hello")));

            assertEquals(
                actorId + ":json-hello",
                awaitActorRelay(
                    actorId + ":json-hello",
                    2,
                    TimeUnit.SECONDS));
        }
    }

    private static ZLinkFrameworkRuntime startLocalJsonRuntime() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addHandlersFromPackageOf(JsonSessionActorsRuntimeIntegrationTest.class);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://json-play-" + System.nanoTime()).setRoutingId(RoutingId.from("play-node"));
                node.objects().server().addSpotFactory("SessionActorsRuntimeIntegrationTest.GameSpot", SessionActorsRuntimeIntegrationTest.GameSpot.class, factory -> factory.disableRelocation());
                node.objects().server().addEntrySpot(SessionActorsRuntimeIntegrationTest.GameEntrySpot.class);
                node.objects().server().addActorFactory(
                    "player",
                    SessionActorsRuntimeIntegrationTest.PlayerActor.class,
                    SessionActorsRuntimeIntegrationTest.PlayerActorFactory.class,
                    factory -> factory.disableRelocation()); }
        { var stream = options.addStreamNode("local-json"); stream.bind("inproc://local-json-bind-" + System.nanoTime());
            stream.enableActorDispatch();
            stream.registerSession(SessionActorsRuntimeIntegrationTest.GameSession.class); };

        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    public record JsonRelaySend(String value) {
    }

    public static final class DefaultJsonActorSendHandler {
        @ZLinkSpotActorSend
        public java.util.concurrent.CompletionStage<Void> handle(
            SessionActorsRuntimeIntegrationTest.PlayerActor actor,
            JsonRelaySend request) {
            SessionActorsRuntimeIntegrationTest.actorRelayRequests.offer(
                actor.context().actorId() + ":" + request.value());
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
    }

    private static String awaitActorRelay(
        String expected,
        long timeout,
        TimeUnit unit) throws Exception {
        long deadline = System.nanoTime() + unit.toNanos(timeout);
        while (true) {
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) {
                throw new java.util.concurrent.TimeoutException();
            }
            String received = SessionActorsRuntimeIntegrationTest.actorRelayRequests.poll(
                remaining,
                TimeUnit.NANOSECONDS);
            if (expected.equals(received)) {
                return received;
            }
        }
    }
}
