package systems.zlink.framework.channels;

import java.util.Map;
import java.util.concurrent.CompletionStage;

public interface ZLinkPublishCall {
    default ZLinkPublishCall metadata(String key, String value) {
        throw new UnsupportedOperationException("publish metadata is not available");
    }

    default ZLinkPublishCall metadata(Map<String, String> metadata) {
        throw new UnsupportedOperationException("publish metadata is not available");
    }

    /**
     * Completes after submission to each local transport or Spot queue has
     * been attempted. It does not wait for a remote queue or handler.
     */
    CompletionStage<Void> submit();
}
