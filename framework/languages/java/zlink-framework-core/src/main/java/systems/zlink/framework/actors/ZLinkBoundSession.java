package systems.zlink.framework.actors;

import java.util.concurrent.CompletionStage;

public interface ZLinkBoundSession {
    ZLinkBoundSessionSendCall send(Object message);

    CompletionStage<Void> disconnect();
}
