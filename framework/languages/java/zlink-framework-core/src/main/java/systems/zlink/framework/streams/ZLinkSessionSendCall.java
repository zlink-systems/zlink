package systems.zlink.framework.streams;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

public interface ZLinkSessionSendCall {
    ZLinkSessionSendCall metadata(String key, String value);

    ZLinkSessionSendCall compress();

    ZLinkSessionSendCall timeout(Duration timeout);

    CompletionStage<Void> submit();
}
