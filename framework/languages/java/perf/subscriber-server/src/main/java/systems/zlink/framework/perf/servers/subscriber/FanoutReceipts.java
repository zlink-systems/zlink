package systems.zlink.framework.perf.servers.subscriber;

import systems.zlink.framework.monitoring.ZLinkFanoutRuntime;
import systems.zlink.framework.perf.CellDirectory;
import systems.zlink.framework.perf.DecimalText;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.FanoutSupport;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.NullReason;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PerfPublishEvent;
import systems.zlink.framework.perf.PerfSnapshot;
import systems.zlink.framework.perf.PerfValidationException;
import systems.zlink.framework.perf.Polling;
import systems.zlink.framework.perf.ProcessSampler;
import systems.zlink.framework.perf.RoleConfig;

import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLong;

// §10.11 Subscriber evidence: which measured sequences this process received first inside its own window or settle (§15.4).
// It counts nothing the Publisher published; the runner intersects both originals.
public final class FanoutReceipts {
    private static final class Round {
        final FanoutSupport.SequenceBitSet window = new FanoutSupport.SequenceBitSet();
        final FanoutSupport.SequenceBitSet settle = new FanoutSupport.SequenceBitSet();
        final AtomicLong duplicates = new AtomicLong();
        final AtomicLong warmupEvents = new AtomicLong();
        final AtomicLong ignoredWarmupInMeasured = new AtomicLong();
        final AtomicLong outsideWindow = new AtomicLong();
        volatile boolean sealed; // set when the runner collects the final snapshot: later events are missing deliveries
    }

    private final ZLinkFanoutRuntime fanoutRuntime;
    private final Measurement measurement;
    private final ObjectsReadiness objects;
    private final RoleConfig config;
    private final Path sequenceFile;
    private volatile Round round = new Round();

    public FanoutReceipts(ZLinkFanoutRuntime fanoutRuntime, Measurement measurement, ObjectsReadiness objects,
            CellDirectory cellDirectory) {
        this.fanoutRuntime = fanoutRuntime;
        this.measurement = measurement;
        this.objects = objects;
        this.config = measurement.config();
        this.sequenceFile = cellDirectory.path().resolve("subscriber-" + config.roleInstance() + "-sequences.json");
        measurement.onReset(() -> round = new Round());
        measurement.messageTypes(List.<String[]>of(new String[] {"event", "PerfPublishEvent"}));
        measurement.enrichSnapshot(this::enrich);
    }

    /** objectsReady: this Subscriber's public fanout status shows a Ready publisher (§16.1). */
    public CompletionStage<Void> prepare() {
        return Polling.until(() -> {
            var status = fanoutRuntime.snapshot(config.channelName());
            return status.isReady() && status.readyPublisherCount() > 0;
        }, 10, config.workload().setupTimeoutMs()).thenAccept(ignored -> objects.set(true, "", List.of(Evidence.of(
                "fanoutStatus", "ZLinkFanoutRuntime.snapshot", fanoutRuntime.snapshot(config.channelName()))))).exceptionally(error -> {
                    measurement.recordDiagnostic(error);
                    return null;
                });
    }

    public void record(PerfPublishEvent message) {
        Round current = round;
        if (!config.runId().equals(message.runId()) || !config.cellId().equals(message.cellId())
                || !FanoutSupport.TOPIC.equals(message.topic())
                || !("warmup".equals(message.phase()) || "measured".equals(message.phase()))
                || message.clockDomainId() == null || message.clockDomainId().isEmpty()) {
            throw new PerfValidationException("IdentityMismatch", "Fanout event identity does not match the cell.");
        }
        long sequence = DecimalText.u64(message.sequence());
        DecimalText.i64(message.sentTicks());
        measurement.pattern().validate(message.payload());
        if ("warmup".equals(message.phase())) {
            if (DecimalText.u64(message.resetSeq()) != 0) {
                throw new PerfValidationException("PhaseMismatch", "Warmup event carries a measured resetSeq.");
            }
            current.warmupEvents.incrementAndGet();
            if (measurement.setupEvidence().isEmpty()) {
                measurement.setupEvidence(List.of(Evidence.of("warmupMarker", "ZLinkFanoutHandler<PerfPublishEvent>", message.sequence())));
            }
            if (!"0".equals(measurement.resetSeq())) {
                current.ignoredWarmupInMeasured.incrementAndGet();
            }
            return;
        }
        if (!message.resetSeq().equals(measurement.resetSeq())) {
            throw new PerfValidationException("PhaseMismatch", "Measured event resetSeq differs from this epoch.");
        }
        // The runner ends the settle (§4.1): receipts after the window are settle until it reads the final snapshot.
        FanoutSupport.SequenceBitSet target = null;
        if (!current.sealed) {
            switch (measurement.phase()) {
                case "measured" -> target = current.window;
                case "settle", "complete" -> target = current.settle;
                default -> target = null;
            }
        }
        if (target == null) {
            current.outsideWindow.incrementAndGet();
        } else if (current.window.contains(sequence) || current.settle.contains(sequence) || !target.trySet(sequence)) {
            current.duplicates.incrementAndGet();
        }
    }

    private void enrich(PerfSnapshot snapshot) {
        Round current = round;
        FanoutSupport.applyCommon(snapshot, true);
        FanoutSupport.value(snapshot, "fanout.duplicateEvents", DecimalText.of(current.duplicates.get()));
        Map<String, Object> receipts = new LinkedHashMap<>();
        receipts.put("uniqueInWindow", DecimalText.of(current.window.count()));
        receipts.put("uniqueInSettle", DecimalText.of(current.settle.count()));
        receipts.put("warmupEvents", DecimalText.of(current.warmupEvents.get()));
        receipts.put("warmupInMeasuredEpoch", DecimalText.of(current.ignoredWarmupInMeasured.get()));
        receipts.put("measuredOutsideWindow", DecimalText.of(current.outsideWindow.get()));
        snapshot.runtimeMetrics.put("fanoutReceipts", ProcessSampler.named("subscriber receipts", "event", "object", receipts));
        Map<String, Object> evidence = new LinkedHashMap<>();
        evidence.put("method", "one bit per received sequence, window and settle sets");
        evidence.put("retainedBytes", DecimalText.of(current.window.retainedBytes() + current.settle.retainedBytes()));
        evidence.put("timingEvidence", "not collected: no shared clock domain");
        evidence.put("original", "subscriber-" + config.roleInstance() + "-sequences.json");
        Map<String, Object> fanoutProvenance = new LinkedHashMap<>();
        fanoutProvenance.put("channelName", config.channelName());
        fanoutProvenance.put("topic", FanoutSupport.TOPIC);
        fanoutProvenance.put("subscribedTopics", List.of());
        fanoutProvenance.put("delivery", "typed ZLinkFanoutHandler<PerfPublishEvent>");
        fanoutProvenance.put("sequenceEvidence", evidence);
        snapshot.provenance.put("fanout", fanoutProvenance);
        if (!measurement.finalSnapshot() || !"complete".equals(snapshot.phase) || !"1".equals(snapshot.resetSeq)) {
            return;
        }
        current.sealed = true;
        Map<String, NullReason> reasons = new LinkedHashMap<>();
        reasons.put("/timingEvidence", new NullReason("CLOCK_DOMAIN_UNVERIFIED",
                "Publisher and Subscriber use process-local monotonic clocks; no shared clock domain is verified (§15.2)."));
        FanoutSupport.writeOnce(sequenceFile, new FanoutSupport.SubscriberSequences(config.runId(), config.cellId(),
                snapshot.resetSeq, "measured", config.roleInstance(), current.window.ranges(), current.settle.ranges(),
                DecimalText.of(current.duplicates.get()), reasons, null));
    }

}
