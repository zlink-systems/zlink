package systems.zlink.framework.runtime.streams;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.completion.ZLinkTerminalWinner;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.spots.ZLinkSpotIdValidator;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkStreamAuthenticationAdmission {
    private static final Map<String, String> NO_TAGS = Map.of();

    record Settings(String packetName, int limit, int retryDelayMs, QueueSettings queue) {}

    record QueueSettings(String key, String previousKey, String meshName, Integer globalLimit) {
        QueueSettings resolveMesh(List<MeshNodeRegistration> meshNodes) {
            if (meshNodes.isEmpty()) {
                throw new ZLinkConfigurationException(
                        "STREAM admission queue requires a configured RouteMesh");
            }
            QueueSettings resolved = this;
            if (meshName == null) {
                if (meshNodes.size() != 1) {
                    throw new ZLinkConfigurationException("Mesh selection is required");
                }
                resolved =
                        new QueueSettings(
                                key, previousKey, meshNodes.getFirst().meshName(), globalLimit);
            } else if (meshNodes.stream().noneMatch(mesh -> mesh.meshName().equals(meshName))) {
                throw new ZLinkConfigurationException("AdmissionQueueMeshName is not configured.");
            }
            try {
                ZLinkSpotIdValidator.requireValid(resolved.spotId());
            } catch (IllegalArgumentException error) {
                throw new ZLinkConfigurationException("Invalid admission queue SpotId.", error);
            }
            return resolved;
        }

        String spotId() {
            return "$zlink.admission-queue/" + meshName;
        }

        @Override
        public String toString() {
            return "QueueSettings[globalLimit=" + globalLimit + "]";
        }
    }

    static Settings settings(
            List<StreamNodeRegistration> registrations, List<MeshNodeRegistration> meshNodes) {
        int processors = Runtime.getRuntime().availableProcessors();
        Settings settings = registrations.getFirst().authenticationAdmission(processors);
        for (int index = 1; index < registrations.size(); index++) {
            if (!settings.equals(registrations.get(index).authenticationAdmission(processors))) {
                throw new ZLinkConfigurationException(
                        "STREAM registrations on a node must use the same authentication admission"
                                + " settings.");
            }
        }
        return settings.queue() == null
                ? settings
                : new Settings(
                        settings.packetName(),
                        settings.limit(),
                        settings.retryDelayMs(),
                        settings.queue().resolveMesh(meshNodes));
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

    QueueSettings queue() {
        return settings.queue();
    }

    int limit() {
        return settings.limit();
    }

    int inFlight() {
        return inFlight.get();
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
