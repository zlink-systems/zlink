package systems.zlink.framework.runtime.internal.metrics;

import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationStoreFailure;

import java.util.Map;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicLong;

/** Registration-bounded Instance Spot accounting, independent of status subscriptions. */
public final class ZLinkInstanceSpotMetrics implements ZLinkRuntimeMetrics.Registration {
    private final Map<String, String> tags;
    private final Map<Outcome, Map<String, String>> outcomes;
    private final AtomicLong messages = new AtomicLong();
    private final AtomicLong bytes = new AtomicLong();
    private final ZLinkRuntimeMetrics.Registration messageRegistration;
    private final ZLinkRuntimeMetrics.Registration byteRegistration;

    private enum Outcome {
        READY,
        REJECTED,
        CONFLICT,
        TIMED_OUT,
        SHUTDOWN,
        STORE_FAILURE,
        FENCED
    }

    public ZLinkInstanceSpotMetrics(String meshName, String stableType) {
        tags =
                Map.of(
                        ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                        meshName,
                        ZLinkRuntimeMetrics.Tag.INSTANCE_SPOT_TYPE.wire(),
                        stableType);
        var values = new java.util.EnumMap<Outcome, Map<String, String>>(Outcome.class);
        for (var outcome : Outcome.values())
            values.put(
                    outcome,
                    Map.of(
                            ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                            meshName,
                            ZLinkRuntimeMetrics.Tag.INSTANCE_SPOT_TYPE.wire(),
                            stableType,
                            ZLinkRuntimeMetrics.Tag.OUTCOME.wire(),
                            ZLinkRuntimeMetrics.wire(outcome)));
        outcomes = Map.copyOf(values);
        messageRegistration =
                ZLinkRuntimeMetrics.registerObservable(
                        ZLinkRuntimeMetrics.Metric.INSTANCE_SPOT_PENDING_MESSAGES,
                        tags,
                        messages::get);
        byteRegistration =
                ZLinkRuntimeMetrics.registerObservable(
                        ZLinkRuntimeMetrics.Metric.INSTANCE_SPOT_PENDING_BYTES, tags, bytes::get);
    }

    public void retain(long payloadBytes) {
        messages.incrementAndGet();
        bytes.addAndGet(payloadBytes);
    }

    public void release(long payloadBytes) {
        messages.decrementAndGet();
        bytes.addAndGet(-payloadBytes);
    }

    public void terminal(long started, Throwable failure) {
        while (failure instanceof CompletionException && failure.getCause() != null)
            failure = failure.getCause();
        Outcome outcome =
                failure == null
                        ? Outcome.READY
                        : failure instanceof ZLinkLocationStoreFailure
                                ? Outcome.STORE_FAILURE
                                : failure instanceof TimeoutException
                                        ? Outcome.TIMED_OUT
                                        : failure instanceof ZLinkFrameworkException typed
                                                ? switch (typed.kind()) {
                                                    case DEADLINE_EXCEEDED -> Outcome.TIMED_OUT;
                                                    case SHUTTING_DOWN -> Outcome.SHUTDOWN;
                                                    case ALREADY_EXISTS, TYPE_MISMATCH ->
                                                            Outcome.CONFLICT;
                                                    case INVALID_OPERATION -> Outcome.FENCED;
                                                    default -> Outcome.REJECTED;
                                                }
                                                : Outcome.REJECTED;
        var terminal = outcomes.get(outcome);
        ZLinkRuntimeMetrics.increment(
                ZLinkRuntimeMetrics.Metric.INSTANCE_SPOT_ACTIVATIONS.metricName(), terminal);
        ZLinkRuntimeMetrics.record(
                ZLinkRuntimeMetrics.Metric.INSTANCE_SPOT_ACTIVATION_DURATION.metricName(),
                Math.max(0L, System.nanoTime() - started) / 1_000_000_000d,
                terminal);
    }

    public void conflict(String reason) {
        ZLinkRuntimeMetrics.increment(
                ZLinkRuntimeMetrics.Metric.INSTANCE_SPOT_CLAIM_CONFLICTS.metricName(),
                Map.of(
                        ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                        tags.get(ZLinkRuntimeMetrics.Tag.MESH_NAME.wire()),
                        ZLinkRuntimeMetrics.Tag.INSTANCE_SPOT_TYPE.wire(),
                        tags.get(ZLinkRuntimeMetrics.Tag.INSTANCE_SPOT_TYPE.wire()),
                        ZLinkRuntimeMetrics.Tag.REASON.wire(),
                        reason));
    }

    @Override
    public void close() {
        messageRegistration.close();
        byteRegistration.close();
    }
}
