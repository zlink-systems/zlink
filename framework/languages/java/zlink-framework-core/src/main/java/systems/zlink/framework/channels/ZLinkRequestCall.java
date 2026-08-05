package systems.zlink.framework.channels;

import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletionStage;

public interface ZLinkRequestCall {
    default ZLinkRequestCall metadata(String key, String value) {
        throw new UnsupportedOperationException("request metadata is not available");
    }

    default ZLinkRequestCall metadata(Map<String, String> metadata) {
        throw new UnsupportedOperationException("request metadata is not available");
    }

    ZLinkRequestCall timeout(Duration timeout);

    <TReply> CompletionStage<TReply> submit(Class<TReply> replyType);

    <TReply> CompletionStage<TReply> yield(Class<TReply> replyType);

}
