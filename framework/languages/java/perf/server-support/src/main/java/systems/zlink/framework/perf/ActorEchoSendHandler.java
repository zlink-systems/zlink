package systems.zlink.framework.perf;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;

import java.util.List;
import java.util.concurrent.CompletionStage;

// §10.10: the Actor answers a send with a public Channel send to the caller's return ChannelName (in the DTO).
public final class ActorEchoSendHandler
        implements ZLinkEntrySpotActorSendHandler<PerfEntrySpot, PerfActor, PerfEchoRequest> {
    private final Measurement measurement;
    private final RoleConfig config;
    private final ZLinkRouteClient route;

    public ActorEchoSendHandler(Measurement measurement, RoleConfig config, ZLinkRouteClient route) {
        this.measurement = measurement;
        this.config = config;
        this.route = route;
    }

    @Override
    public CompletionStage<Void> handle(PerfEntrySpot spot, PerfActor actor, ZLinkMessageContext context,
            PerfEchoRequest request) {
        long received = PerfClock.now();
        measurement.handlerEnter();
        CompletionStage<Void> sent;
        try {
            measurement.validateRequest(request, config.channelName(), null);
            PerfEchoReply reply = PayloadPattern.reply(request, received);
            measurement.recordApplicationCall(request, "send");
            sent = route.sendToChannel(request.returnChannel(), reply).submit().whenComplete((ignored, error) -> {
                if (error == null && "setup".equals(measurement.phase())) {
                    measurement.setupEvidence(List.of(Evidence.of("typedProbeReply",
                            "ZLinkRouteClient.sendToChannel(returnChannel).submit", request.correlationId())));
                }
                if (error != null) {
                    measurement.recordDiagnostic(error);
                }
                measurement.handlerExit();
            });
        } catch (RuntimeException error) {
            measurement.recordDiagnostic(error);
            measurement.handlerExit();
            throw error;
        }
        return sent;
    }
}
