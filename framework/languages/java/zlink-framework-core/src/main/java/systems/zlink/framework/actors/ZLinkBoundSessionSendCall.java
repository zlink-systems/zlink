package systems.zlink.framework.actors;

import java.util.concurrent.CompletionStage;

public interface ZLinkBoundSessionSendCall {
    ZLinkBoundSessionSendCall metadata(String key, String value);

    CompletionStage<Void> submit();
}
