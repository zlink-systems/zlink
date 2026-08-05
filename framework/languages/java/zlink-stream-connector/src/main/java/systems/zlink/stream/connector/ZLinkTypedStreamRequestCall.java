package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletionStage;

public interface ZLinkTypedStreamRequestCall {
    ZLinkTypedStreamRequestCall packetName(String name);

    ZLinkTypedStreamRequestCall metadata(String key, String value);

    ZLinkTypedStreamRequestCall metadata(Map<String, String> metadata);

    ZLinkTypedStreamRequestCall timeout(Duration timeout);

    ZLinkTypedStreamRequestCall compress();

    <TReply> CompletionStage<TReply> submit(Class<TReply> replyType);
}
