package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.springframework.context.SmartLifecycle;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory;

import java.util.concurrent.*;

final class PreparedCallLifecycleTest {
    @Test
    void constructorCallIsRejectedAndHostedStartCallUsesTheSameFacade() {
        try (var context = new AnnotationConfigApplicationContext()) {
            context.registerBean(ZLinkFrameworkEnabled.class, ZLinkFrameworkEnabled::new);
            context.registerBean(
                    ZLinkBackendAdapterProvider.class, FakeZLinkBackendAdapterFactory::new);
            context.registerBean(
                    ZLinkFrameworkConfigurer.class,
                    () ->
                            options ->
                                    options.addFanoutChannel("fanout")
                                            .enablePublisher("inproc://hosted-prepared")
                                            .setRoutingId(RoutingId.from("publisher")));
            context.registerBean(CallingService.class);
            context.register(ZLinkFrameworkAutoConfiguration.class);
            context.refresh();
            var service = context.getBean(CallingService.class);
            assertTrue(service.isRunning());
            assertTrue(context.getBean(ZLinkFrameworkRuntime.class).isReady());
            var failure =
                    assertThrows(
                            CompletionException.class,
                            () -> service.before.toCompletableFuture().join());
            assertEquals(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            for (var before : service.beforeObjects)
                rejected(before, ZLinkFrameworkErrorKind.INVALID_OPERATION);
            var runtime = context.getBean(ZLinkFrameworkRuntime.class);
            var actor = runtime.actorClient().sendToActor("actor", "message");
            var spot = runtime.route().sendToSpot("spot", "message");
            var publisher =
                    runtime.spotPublisherClient().publish("mesh", "channel", "topic", "message");
            context.close();
            rejected(
                    service.client.publish("fanout", "topic", "after").submit(),
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            rejected(service.stopped.submit(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            rejected(service.stopped.submit(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            rejected(actor.submit(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            rejected(spot.submit(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            rejected(publisher.submit(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            rejected(runtime.actorManager().find("actor"), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            rejected(runtime.spotManager().find("spot"), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
        }
    }

    private static void rejected(CompletionStage<?> stage, ZLinkFrameworkErrorKind kind) {
        var failure =
                assertThrows(CompletionException.class, () -> stage.toCompletableFuture().join());
        assertEquals(
                kind, assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }

    static final class CallingService implements SmartLifecycle {
        private final ZLinkFanoutClient client;
        final CompletionStage<Void> before;
        final java.util.List<CompletionStage<?>> beforeObjects;
        final systems.zlink.framework.channels.ZLinkFanoutPublishCall reused;
        final systems.zlink.framework.channels.ZLinkFanoutPublishCall stopped;
        private boolean running;

        CallingService(ZLinkFanoutClient client, ZLinkFrameworkRuntime runtime) {
            this.client = client;
            beforeObjects =
                    java.util.List.of(
                            runtime.actorClient().sendToActor("actor", "message").submit(),
                            runtime.actorManager().find("actor"),
                            runtime.route().requestToSpot("spot", "message").submit(String.class),
                            runtime.spotManager().find("spot"),
                            runtime.spotPublisherClient()
                                    .publish("mesh", "channel", "topic", "message")
                                    .submit());
            reused = client.publish("fanout", "topic", "before");
            stopped = client.publish("fanout", "topic", "stop");
            before = reused.submit();
        }

        public void start() {
            reused.submit().toCompletableFuture().join();
            client.publish("fanout", "topic", "hosted start").submit().toCompletableFuture().join();
            running = true;
        }

        public void stop() {
            running = false;
        }

        public boolean isRunning() {
            return running;
        }

        public int getPhase() {
            return 1;
        }
    }
}
