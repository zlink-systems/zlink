package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.completion.ZLinkTerminalWinner;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkStreamAuthenticationAdmission {
    private static final Map<String, String> NO_TAGS = Map.of();

    record Settings(String packetName, int limit, int retryDelayMs) {}

    static Settings settings(List<StreamNodeRegistration> registrations) {
        int processors = Runtime.getRuntime().availableProcessors();
        Settings settings = registrations.getFirst().authenticationAdmission(processors);
        for (int index = 1; index < registrations.size(); index++) {
            if (!settings.equals(registrations.get(index).authenticationAdmission(processors))) {
                throw new ZLinkConfigurationException(
                        "STREAM registrations on a node must use the same authentication admission settings.");
            }
        }
        return settings;
    }

    private final Settings settings;
    private final Map<String, String> retryMetadata;
    private final AtomicInteger inFlight = new AtomicInteger();

    ZLinkStreamAuthenticationAdmission(Settings settings) {
        this.settings = settings;
        retryMetadata =
                Map.of("$zlink.admission.retryAfterMs", Integer.toString(settings.retryDelayMs()));
    }

    ZLinkRuntimeMetrics.Registration registerMetric() {
        return ZLinkRuntimeMetrics.registerObservable(
                ZLinkRuntimeMetrics.Metric.HOST_AUTHENTICATION_ADMISSION_IN_FLIGHT,
                NO_TAGS,
                inFlight::get);
    }

    boolean appliesTo(ZLinkStreamHeader header) {
        return header.kind() == ZLinkStreamMessageKind.REQUEST
                && header.packetName().equals(settings.packetName());
    }

    Lease tryAdmit() {
        int current = inFlight.get();
        while (current < settings.limit()) {
            if (inFlight.compareAndSet(current, current + 1)) {
                return new Lease();
            }
            current = inFlight.get();
        }
        ZLinkRuntimeMetrics.increment(
                ZLinkRuntimeMetrics.Metric.HOST_AUTHENTICATION_ADMISSION_REJECTIONS.metricName(),
                NO_TAGS);
        return null;
    }

    Map<String, String> retryMetadata() {
        return retryMetadata;
    }

    final class Lease {
        private final ZLinkTerminalWinner terminal = new ZLinkTerminalWinner();

        boolean isTerminal() {
            return terminal.isTerminal();
        }

        void finish(ZLinkTerminalWinner.Cause cause) {
            if (terminal.tryWin(cause)) {
                inFlight.decrementAndGet();
            }
        }
    }
}
