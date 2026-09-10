package systems.zlink.framework.channels;

import java.util.Map;
import java.util.concurrent.CompletionStage;

public interface ZLinkSendCall {
    default ZLinkSendCall metadata(String key, String value) {
        throw new UnsupportedOperationException("send metadata is not available");
    }

    default ZLinkSendCall metadata(Map<String, String> metadata) {
        throw new UnsupportedOperationException("send metadata is not available");
    }

    /** For {@code sendToChannel}, channel validation (including metadata support) and any default readiness timeout are resolved when {@code submit} is called. */
    CompletionStage<Void> submit();
}
