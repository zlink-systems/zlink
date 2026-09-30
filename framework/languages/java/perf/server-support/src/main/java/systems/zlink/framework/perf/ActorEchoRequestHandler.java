package systems.zlink.framework.perf;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// §10.1, §10.2, §10.9: the Actor's typed request handler returns the typed echo; that return value is the reply.
public final class ActorEchoRequestHandler
        implements ZLinkEntrySpotActorRequestHandler<PerfEntrySpot, PerfActor, PerfEchoRequest, PerfEchoReply> {
    private final Measurement measurement;

    public ActorEchoRequestHandler(Measurement measurement) {
        this.measurement = measurement;
    }

    @Override
    public CompletionStage<PerfEchoReply> handle(PerfEntrySpot spot, PerfActor actor, ZLinkMessageContext context,
            PerfEchoRequest request) {
        long received = PerfClock.now();
        measurement.handlerEnter();
        try {
            measurement.validateRequest(request);
            PerfEchoReply reply = PayloadPattern.reply(request, received);
            measurement.recordReply(request);
            if ("setup".equals(measurement.phase())) {
                measurement.setupEvidence(List.of(Evidence.of("typedProbeReply",
                        "ZLinkEntrySpotActorRequestHandler<PerfEntrySpot,PerfActor,PerfEchoRequest,PerfEchoReply>",
                        request.correlationId())));
            }
            return CompletableFuture.completedFuture(reply);
        } catch (RuntimeException error) {
            measurement.recordDiagnostic(error);
            throw error;
        } finally {
            measurement.handlerExit();
        }
    }
}
