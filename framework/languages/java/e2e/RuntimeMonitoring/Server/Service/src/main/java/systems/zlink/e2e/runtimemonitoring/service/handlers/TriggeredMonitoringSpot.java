package systems.zlink.e2e.runtimemonitoring.service.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;

public final class TriggeredMonitoringSpot implements ZLinkSpot<ZLinkActor> {
    private final ZLinkSpotContext context;

    public TriggeredMonitoringSpot(ZLinkSpotContext context) {
        this.context = context;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(SubjectProbeHandler.class);
    }

    @Override
    public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
        return CompletableFuture.completedFuture(null);
    }

    @ZLinkSpotSubscription(topic = "monitoring.subject.trigger")
    public static final class SubjectProbeHandler
        implements ZLinkSpotSubscriptionHandler<TriggeredMonitoringSpot, Contracts.SpotSubjectProbe> {
        @Override
        public CompletionStage<Void> handle(
            TriggeredMonitoringSpot spot,
            Contracts.SpotSubjectProbe event) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
