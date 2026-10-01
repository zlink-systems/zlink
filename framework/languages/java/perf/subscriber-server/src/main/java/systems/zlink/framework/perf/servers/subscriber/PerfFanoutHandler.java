package systems.zlink.framework.perf.servers.subscriber;

import systems.zlink.framework.channels.ZLinkFanoutHandler;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfPublishEvent;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

// §10.11 Subscriber: the typed fanout handler validates each event and records its unique sequence.
public final class PerfFanoutHandler implements ZLinkFanoutHandler<PerfPublishEvent> {
    private final Measurement measurement;
    private final FanoutReceipts receipts;

    public PerfFanoutHandler(Measurement measurement, FanoutReceipts receipts) {
        this.measurement = measurement;
        this.receipts = receipts;
    }

    @Override
    public CompletionStage<Void> handle(PerfPublishEvent message, ZLinkPublishMessageContext context) {
        long handlerEntryTicks = PerfClock.now();
        measurement.handlerEnter();
        try {
            receipts.record(message, handlerEntryTicks);
        } catch (RuntimeException error) {
            measurement.recordDiagnostic(error);
            throw error;
        } finally {
            measurement.handlerExit();
        }
        return CompletableFuture.completedFuture(null);
    }
}
