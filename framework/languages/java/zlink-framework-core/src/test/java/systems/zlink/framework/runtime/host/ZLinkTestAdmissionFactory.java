package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Supplier;

public final class ZLinkTestAdmissionFactory {
    public interface Backend {
        default ZLinkBackendObject admissionSource() {
            return (ZLinkBackendObject) this;
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
                                ? CompletableFuture.completedFuture(null)
                                : CompletableFuture.failedFuture(
                                        new IllegalStateException(
                                                "one-way submission was not admitted"));
                    } finally {
                        cleanup.run();
                    }
                };
    }

    /** Completes a fake request's admission when its reply is sent. */
    public static <T> CompletionStage<T> acceptedRequest(
            CompletionStage<T> reply, CompletableFuture<Void> admission) {
        if (admission != null)
            reply.whenComplete(
                    (ignored, failure) -> {
                        if (failure == null) admission.complete(null);
                        else admission.completeExceptionally(failure);
                    });
        return reply;
    }
}
