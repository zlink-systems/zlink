package systems.zlink.framework.runtime.internal.backend;

import java.util.concurrent.CompletionStage;

/**
 * Keeps native claim completion inside the Java backend without changing the public backend
 * dispatch callback contract.
 */
public interface ZLinkInternalAsyncSpotDispatchHandler extends ZLinkBackendSpotDispatchHandler {
    CompletionStage<Void> handleAsync(ZLinkBackendSpotDispatchInfo info);

    @Override
    default void handle(ZLinkBackendSpotDispatchInfo info) {
        handleAsync(info);
    }
}
