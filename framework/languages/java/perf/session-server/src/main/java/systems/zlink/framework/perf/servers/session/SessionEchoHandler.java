package systems.zlink.framework.perf.servers.session;

import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;

import java.util.List;
import java.util.concurrent.CompletionStage;

// §11.1: the typed session handler answers the STREAM request with client().reply(reply).submit().
public final class SessionEchoHandler implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, PerfEchoRequest> {
    private final Measurement measurement;

    public SessionEchoHandler(Measurement measurement) {
        this.measurement = measurement;
    }

    @Override
    public Class<PerfEchoRequest> messageType() {
        return PerfEchoRequest.class;
    }

    @Override
    public CompletionStage<Void> handle(ZLinkSessionContext context, ZLinkSessionDispatchContext dispatch,
            PerfEchoRequest request) {
        long received = PerfClock.now();
        measurement.handlerEnter();
        try {
            measurement.validateRequest(request);
            PerfEchoReply reply = PayloadPattern.reply(request, received);
            measurement.recordReply(request);
            return context.client().reply(reply).submit().whenComplete((ignored, error) -> {
                if (error == null && "setup".equals(measurement.phase())) {
                    measurement.setupEvidence(List.of(Evidence.of("typedProbeReply", "SessionEchoHandler.client().reply().submit",
                            request.correlationId())));
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
