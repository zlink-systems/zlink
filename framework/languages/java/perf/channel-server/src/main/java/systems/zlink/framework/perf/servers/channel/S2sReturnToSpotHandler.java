package systems.zlink.framework.perf.servers.channel;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.PerfValidationException;

import java.util.List;
import java.util.concurrent.CompletionStage;

// §10.6: the Channel handler answers with `sendToSpot(returnSpotId, reply)`, where the DTO names the return SpotId.
public final class S2sReturnToSpotHandler implements ZLinkSendHandler<PerfEchoRequest> {
    private final Measurement measurement;
    private final ZLinkRouteClient spots;

    public S2sReturnToSpotHandler(Measurement measurement, ZLinkRouteClient spots) {
        this.measurement = measurement;
        this.spots = spots;
    }

    @Override
    public CompletionStage<Void> handle(PerfEchoRequest message, ZLinkMessageContext context) {
        long received = PerfClock.now();
        measurement.handlerEnter();
        try {
            if (message.returnSpotId() == null || message.returnSpotId().isEmpty()) {
                throw new PerfValidationException("IdentityMismatch", "No return SpotId in the request.");
            }
            measurement.validateRequest(message, null, message.returnSpotId());
            PerfEchoReply reply = PayloadPattern.reply(message, received);
            measurement.recordApplicationCall(message, "send");
            return spots.sendToSpot(message.returnSpotId(), reply).submit().whenComplete((ignored, error) -> {
                if (error == null && "setup".equals(measurement.phase())) {
                    measurement.setupEvidence(List.of(Evidence.of("typedProbeReply",
                            "ZLinkSendHandler<PerfEchoRequest> -> ZLinkRouteClient.sendToSpot", message.correlationId())));
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
    }
}
