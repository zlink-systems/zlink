package systems.zlink.framework.channels;

import java.util.concurrent.CompletionStage;

/** Observes a request reply and its source-local admission. */
public interface ZLinkRequestStage<TReply> extends CompletionStage<TReply> {
    CompletionStage<Void> admitted();
}
