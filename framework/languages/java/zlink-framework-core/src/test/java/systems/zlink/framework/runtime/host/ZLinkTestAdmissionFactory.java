package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

import java.time.Duration;
import java.util.concurrent.CompletionStage;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Supplier;

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

        default void setAdmissionReadyHandler(Consumer<ZLinkBackendAdmissionKey> handler) {}

        default void setAdmissionShutdownHandler(Runnable handler) {}
    }

    private ZLinkTestAdmissionFactory() {}

    public static BiFunction<
                    ZLinkBackendObject,
                    ZLinkBackendAdmissionKey,
                    BiFunction<Supplier<Boolean>, Runnable, CompletionStage<Void>>>
            create() {
        return (backend, key) ->
                (submission, cleanup) -> {
                    try {
                        return submission.get()
                                ? java.util.concurrent.CompletableFuture.completedFuture(null)
                                : java.util.concurrent.CompletableFuture.failedFuture(
                                        new IllegalStateException(
                                                "one-way submission was not admitted"));
                    } finally {
                        cleanup.run();
                    }
                };
    }
}
