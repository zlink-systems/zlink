package systems.zlink.stream.connector;

import java.util.concurrent.CompletionStage;

public interface ZLinkStreamLifecycleCall {
    CompletionStage<Void> submit();
}
