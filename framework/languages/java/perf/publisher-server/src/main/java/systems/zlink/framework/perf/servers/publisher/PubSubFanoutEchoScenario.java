package systems.zlink.framework.perf.servers.publisher;

import org.springframework.beans.factory.ObjectProvider;

import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.perf.CellDirectory;
import systems.zlink.framework.perf.CompletionLoop;
import systems.zlink.framework.perf.DecimalText;
import systems.zlink.framework.perf.Evidence;
import systems.zlink.framework.perf.FanoutSupport;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfPublishEvent;
import systems.zlink.framework.perf.PerfSnapshot;
import systems.zlink.framework.perf.Polling;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ServerApplication;
import systems.zlink.framework.perf.Streams;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLong;

// §10.11 Publisher: one process issues every sequence of the run. Each logical stream awaits the public publish admission
// (ZLinkFanoutClient.publish(...).submit()); nothing waits for a subscriber. Delivery is not observed here: the Subscribers'
// own originals are intersected with this process's window-success set by the runner (§15.4).
public final class PubSubFanoutEchoScenario {
    private static final class PublishedSets {
        final FanoutSupport.SequenceBitSet windowSuccess = new FanoutSupport.SequenceBitSet();
    }

    private final ZLinkFanoutClient fanout;
    private final ObjectProvider<ZLinkFrameworkRuntime> runtimeProvider;
    private final Measurement measurement;
    private final ObjectsReadiness objects;
    private final RoleConfig config;
    private final Path sequenceFile;
    private final AtomicLong issued = new AtomicLong(); // run-wide: warmup and measured ranges never overlap
    private volatile PublishedSets sets = new PublishedSets();
    private volatile long measuredBase;

    public PubSubFanoutEchoScenario(ZLinkFanoutClient fanout, ObjectProvider<ZLinkFrameworkRuntime> runtimeProvider,
            Measurement measurement,
            ObjectsReadiness objects, CellDirectory cellDirectory) {
        this.fanout = fanout;
        this.runtimeProvider = runtimeProvider;
        this.measurement = measurement;
        this.objects = objects;
        this.config = measurement.config();
        this.sequenceFile = cellDirectory.path().resolve("publisher-sequences.json");
        measurement.onReset(() -> {
            measuredBase = issued.get();
            sets = new PublishedSets();
        });
        measurement.messageTypes(List.<String[]>of(new String[] {"event", "PerfPublishEvent"}));
        measurement.enrichSnapshot(this::enrich);
    }

    public static void run(RoleConfig config, java.nio.file.Path cellDirectory) {
        // Automatic Classic fanout: the publisher listens on the reserved endpoint and publishes its descriptor to the run's Store.
        ServerApplication app = ServerApplication.create(config)
                .configure(options -> options.addFanoutChannel(config.channelName()).setRoutingIdPrefix("perf-publisher")
                        .enablePublisher(config.transportEndpoints().get("fanout")))
                .bean(ObjectsReadiness.class, () -> new ObjectsReadiness(false, "The Publisher host is not Ready yet."))
                .bean(CellDirectory.class, () -> new CellDirectory(cellDirectory))
                .bean(PubSubFanoutEchoScenario.class)
                .workload(PubSubFanoutEchoScenario.class, PubSubFanoutEchoScenario::run);
        app.start().getBean(PubSubFanoutEchoScenario.class).prepare();
    }

    // The Publisher has no subscriber-facing status: its host Ready plus the Subscribers' public Ready (fanout runtime status,
    // one per Subscriber process) is the prepared state.
    public CompletionStage<Void> prepare() {
        ZLinkFrameworkRuntime runtime = runtimeProvider.getObject();
        return Polling.until(() -> runtime.status().isReady(), 10, config.workload().setupTimeoutMs()).thenAccept(ignored -> {
            var status = runtimeProvider.getObject().status();
            Map<String, Object> observed = new LinkedHashMap<>();
            observed.put("state", status.state());
            observed.put("isReady", status.isReady());
            observed.put("acceptingWork", status.acceptingWork());
            objects.set(true, "", List.of(Evidence.of("publisherHostReady", "ZLinkFrameworkRuntime.status", observed)));
        }).exceptionally(error -> {
            measurement.recordDiagnostic(error);
            return null;
        });
    }

    public CompletionStage<Void> run() {
        return Streams.launch(config, this::step);
    }

    private void step(int stream, CompletableFuture<Void> done) {
        CompletionLoop.run(done, () -> {
            if (!measurement.canIssue()) {
                return Optional.empty();
            }
            long started = measurement.beginOperation("event");
            if (started < 0) {
                return Optional.empty();
            }
            long sequence = issued.incrementAndGet();
            boolean warmup = "0".equals(measurement.resetSeq());
            PublishedSets current = sets;
            PerfPublishEvent message = new PerfPublishEvent(config.runId(), config.cellId(), measurement.resetSeq(),
                    warmup ? "warmup" : "measured", DecimalText.of(sequence), FanoutSupport.TOPIC, DecimalText.of(started),
                    PerfClock.DOMAIN, measurement.pattern().base64());
            CompletionStage<Void> call;
            try {
                call = fanout.publish(config.channelName(), FanoutSupport.TOPIC, message).submit();
            } catch (RuntimeException error) {
                call = CompletableFuture.failedFuture(error);
            }
            return Optional.of(new CompletionLoop.Iteration<>(call, (ignored, error) -> {
                if (error == null) {
                    long completed = PerfClock.now();
                    if (warmup) {
                        measurement.completeOperation(started, null, completed);
                        if (measurement.setupEvidence().isEmpty()) {
                            measurement.setupEvidence(List.of(Evidence.of("warmupMarkerPublished",
                                    "ZLinkFanoutClient.publish.submit", message.sequence())));
                        }
                    } else {
                        if (measurement.completeOperation(started, null, completed)) {
                            current.windowSuccess.trySet(sequence);
                        }
                    }
                } else {
                    measurement.completeOperation(started, error);
                }
            }));
        });
    }

    private void enrich(PerfSnapshot snapshot) {
        PublishedSets current = sets;
        FanoutSupport.applyCommon(snapshot, false);
        FanoutSupport.value(snapshot, "messages.publishedInWindow", DecimalText.of(current.windowSuccess.count()));
        Object seconds = snapshot.window.get("measuredSeconds");
        if (seconds instanceof Double measured && measured > 0) {
            FanoutSupport.value(snapshot, "fanout.publishOpsPerSec", current.windowSuccess.count() / measured);
        } else {
            FanoutSupport.nullValue(snapshot, "fanout.publishOpsPerSec", "PHASE_NOT_STARTED", "No measured window has run.");
        }
        Map<String, Object> fanoutProvenance = new LinkedHashMap<>();
        fanoutProvenance.put("channelName", config.channelName());
        fanoutProvenance.put("topic", FanoutSupport.TOPIC);
        fanoutProvenance.put("noDrop", false);
        fanoutProvenance.put("publisherSequenceScope", "one counter per run; warmup and measured ranges are disjoint");
        fanoutProvenance.put("sequenceOriginal", "publisher-sequences.json");
        snapshot.provenance.put("fanout", fanoutProvenance);
        if (!measurement.finalSnapshot() || !"complete".equals(snapshot.phase) || !"1".equals(snapshot.resetSeq)) {
            return;
        }
        FanoutSupport.writeOnce(sequenceFile, new FanoutSupport.PublisherSequences(config.runId(), config.cellId(),
                snapshot.resetSeq, "measured", issued.get() > measuredBase
                        ? List.of(new FanoutSupport.SequenceRange(DecimalText.of(measuredBase + 1), DecimalText.of(issued.get())))
                        : List.of(), current.windowSuccess.ranges()));
    }
}
