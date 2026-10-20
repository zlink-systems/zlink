package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;

import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkSpotOutboundDispatchBudgetTest {
    @Test
    void spotCallbacksRunInTheClaimedSerialTurnWithoutAnotherExecutorSubmission() throws Exception {
        var submissions = new AtomicInteger();
        var outboundSubmissions = new AtomicInteger();
        var options = new DefaultZLinkFrameworkOptions();
        options.useHandlerExecutor(
                work -> {
                    submissions.incrementAndGet();
                    if (StackWalker.getInstance()
                            .walk(
                                    frames ->
                                            frames.anyMatch(
                                                    frame ->
                                                            frame.getClassName()
                                                                            .equals(
                                                                                    ZLinkSpotRuntime
                                                                                            .class
                                                                                            .getName())
                                                                    && frame.getMethodName()
                                                                            .equals(
                                                                                    "withCurrentOutbound"))))
                        outboundSubmissions.incrementAndGet();
                    work.run();
                });
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRouteMesh("mesh")
                .listen("inproc://outbound-budget-" + UUID.randomUUID())
                .objects()
                .server()
                .addSpotFactory("budget", BudgetSpot.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            int before = submissions.get();
            int outboundBefore = outboundSubmissions.get();
            var created =
                    runtime.spotManager().create("budget").submit().toCompletableFuture().join();
            org.junit.jupiter.api.Assertions.assertNotNull(created.spot());
            org.junit.jupiter.api.Assertions.assertNotNull(created.spot().spotId());
            System.out.println(
                    "Spot create executor submissions="
                            + (submissions.get() - before)
                            + " outbound submissions="
                            + (outboundSubmissions.get() - outboundBefore));
            assertEquals(
                    0,
                    outboundSubmissions.get() - outboundBefore,
                    "claimed Spot creation callbacks must not enqueue another handler task");
        }
    }

    public static final class BudgetSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public BudgetSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
