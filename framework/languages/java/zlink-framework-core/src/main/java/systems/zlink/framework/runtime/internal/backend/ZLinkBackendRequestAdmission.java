package systems.zlink.framework.runtime.internal.backend;

import java.util.concurrent.CompletableFuture;

/** Owns compatibility with backend adapters that expose only the reply terminal. */
final class ZLinkBackendRequestAdmission {
    private ZLinkBackendRequestAdmission() {}

    static void requireUnobserved(CompletableFuture<Void> admission) {
        if (admission != null) {
            throw new UnsupportedOperationException("request admission observation is unavailable");
        }
    }
}
