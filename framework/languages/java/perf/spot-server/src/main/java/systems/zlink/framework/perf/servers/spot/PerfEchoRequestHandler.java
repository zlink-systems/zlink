package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfEchoReply;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class PerfEchoRequestHandler implements ZLinkSpotRequestHandler<PerfEchoSpot, PerfEchoRequest, PerfEchoReply> {
    private final Measurement measurement;
    private final ScenarioMetrics metrics;

    public PerfEchoRequestHandler(Measurement measurement, ScenarioMetrics metrics) {
        this.measurement = measurement;
        this.metrics = metrics;
    }

    @Override
    public CompletionStage<PerfEchoReply> handle(PerfEchoSpot spot, PerfEchoRequest request) {
        long received = PerfClock.now();
        measurement.handlerEnter();
        try {
            measurement.validateRequest(request);
            PerfEchoReply reply = PayloadPattern.reply(request, received);
            measurement.recordReply(request);
            if ("measured".equals(request.phase())) {
                metrics.count("spot.applicationHandlerEntries");
            }
            if ("setup".equals(measurement.phase()) && !measurement.config().source()) {
                measurement.setupEvidence(List.of(Evidence.of("typedProbeReply",
                        "ZLinkSpotRequestHandler<PerfEchoSpot,PerfEchoRequest,PerfEchoReply>", request.correlationId())));
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
