package systems.zlink.framework.runtime.host;

import java.util.concurrent.CompletionStage;
import java.util.function.BiFunction;
import java.util.function.Supplier;
import java.time.Duration;
import java.util.function.Consumer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

public final class ZLinkTestAdmissionFactory {
    public interface Backend {
        default ZLinkBackendObject admissionSource() {
            return (ZLinkBackendObject) this;
        }

        default Duration admissionTimeout() {
            return Duration.ofSeconds(1);
        }

        default int admissionPendingCapacity() {
            return 4096;
        }

        default void setAdmissionReadyHandler(
            Consumer<ZLinkBackendAdmissionKey> handler) {
        }

        default void setAdmissionShutdownHandler(Runnable handler) {
        }
    }

    private ZLinkTestAdmissionFactory() {
    }

    public static BiFunction<
        ZLinkBackendObject,
        ZLinkBackendAdmissionKey,
        BiFunction<Supplier<Boolean>, Runnable, CompletionStage<Void>>> create() {
        return ZLinkAdmissionRuntime.factory(
            backend -> ((Backend) backend).admissionSource(),
            backend -> ((Backend) backend).admissionTimeout(),
            backend -> ((Backend) backend).admissionPendingCapacity(),
            (backend, handler) ->
                ((Backend) backend).setAdmissionReadyHandler(handler),
            (backend, handler) ->
                ((Backend) backend).setAdmissionShutdownHandler(handler));
    }
}
