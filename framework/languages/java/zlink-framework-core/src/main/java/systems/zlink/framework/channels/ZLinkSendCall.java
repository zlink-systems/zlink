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

    CompletionStage<Void> submit();
}
