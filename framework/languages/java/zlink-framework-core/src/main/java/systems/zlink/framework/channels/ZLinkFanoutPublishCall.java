package systems.zlink.framework.channels;

import java.util.concurrent.CompletionStage;

public interface ZLinkFanoutPublishCall {
    CompletionStage<Void> submit();
}
