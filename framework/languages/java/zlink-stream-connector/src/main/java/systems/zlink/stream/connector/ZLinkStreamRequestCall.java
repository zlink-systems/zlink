package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletionStage;

public interface ZLinkStreamRequestCall {
    ZLinkStreamRequestCall packetName(String name);

    ZLinkStreamRequestCall metadata(String key, String value);

    ZLinkStreamRequestCall metadata(Map<String, String> metadata);

    ZLinkStreamRequestCall compress();

    ZLinkStreamRequestCall timeout(Duration timeout);

    CompletionStage<ZLinkStreamEncodedPayload> submit();

    <TReply> CompletionStage<TReply> submit(Class<TReply> replyType);

}
