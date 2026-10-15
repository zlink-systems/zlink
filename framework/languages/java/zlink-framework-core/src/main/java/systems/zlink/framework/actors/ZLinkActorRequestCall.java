package systems.zlink.framework.actors;

import systems.zlink.framework.channels.ZLinkRequestStage;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

public interface ZLinkActorRequestCall {
    ZLinkActorRequestCall metadata(String key, String value);

    ZLinkActorRequestCall timeout(Duration timeout);

    <TReply> ZLinkRequestStage<TReply> submit(Class<TReply> replyType);

    <TReply> CompletionStage<TReply> yield(Class<TReply> replyType);
}
