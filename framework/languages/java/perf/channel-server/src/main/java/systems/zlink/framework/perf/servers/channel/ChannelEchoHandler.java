package systems.zlink.framework.perf.servers.channel;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// The typed Channel request handler of the echo target: §11.2 (both topologies) and §10.5 (the remote Channel process).
public final class ChannelEchoHandler implements ZLinkRequestHandler<PerfEchoRequest, PerfEchoReply> {
    private final Measurement measurement;

    public ChannelEchoHandler(Measurement measurement) {
        this.measurement = measurement;
    }

    @Override
    public CompletionStage<PerfEchoReply> handle(PerfEchoRequest request, ZLinkMessageContext context) {
        long received = PerfClock.now();
        measurement.handlerEnter();
        try {
            measurement.validateRequest(request);
            PerfEchoReply reply = PayloadPattern.reply(request, received);
            measurement.recordReply(request);
            if ("setup".equals(measurement.phase())) {
                measurement.setupEvidence(List.of(Evidence.of("typedProbeReply",
                        "ZLinkRequestHandler<PerfEchoRequest,PerfEchoReply>", request.correlationId())));
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
