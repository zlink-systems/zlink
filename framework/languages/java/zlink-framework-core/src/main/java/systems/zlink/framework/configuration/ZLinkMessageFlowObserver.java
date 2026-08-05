package systems.zlink.framework.configuration;

import java.util.concurrent.CompletionStage;

public interface ZLinkMessageFlowObserver {
    CompletionStage<Void> onMessageFlow(ZLinkMessageFlowEvent flow);
}
