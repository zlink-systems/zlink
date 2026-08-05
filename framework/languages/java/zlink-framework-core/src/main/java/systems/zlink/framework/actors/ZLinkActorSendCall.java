package systems.zlink.framework.actors;

import java.util.concurrent.CompletionStage;

public interface ZLinkActorSendCall {
    ZLinkActorSendCall metadata(String key, String value);

    CompletionStage<Void> submit();
}
