package systems.zlink.e2e.storefailure.provider;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;

public final class LeaseProbeSpot implements ZLinkInstanceSpot {
    private final ZLinkInstanceSpotContext context;
    private final ProviderEvidenceStore evidence;

    public LeaseProbeSpot(
        ZLinkInstanceSpotContext context,
        ProviderEvidenceStore evidence) {
        this.context = context;
        this.evidence = evidence;
    }

    @Override
    public ZLinkInstanceSpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addPacket(LeaseProbeRequestHandler.class);
    }

    @Override
    public CompletionStage<Void> onInitialize() {
        evidence.record("instance-initialize", context.spotId());
        return context.addTimer(
                "owner-lease-probe",
                Duration.ofMillis(100),
                LeaseProbeTimerHandler.class,
                null)
            .thenApply(ignored -> null);
    }

    ProviderEvidenceStore evidence() {
        return evidence;
    }
}
