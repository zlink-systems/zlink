package systems.zlink.framework.streams;

import java.util.concurrent.CompletionStage;

public interface ZLinkSessionReplyCall {
    ZLinkSessionReplyCall compress();

    CompletionStage<Void> submit();
}
