package systems.zlink.framework.actors;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

public interface ZLinkActorRequestCall {
    ZLinkActorRequestCall metadata(String key, String value);

    ZLinkActorRequestCall timeout(Duration timeout);

    <TReply> CompletionStage<TReply> submit(Class<TReply> replyType);

    <TReply> CompletionStage<TReply> yield(Class<TReply> replyType);

}
