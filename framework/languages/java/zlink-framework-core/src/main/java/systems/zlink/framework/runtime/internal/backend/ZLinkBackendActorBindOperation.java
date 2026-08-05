package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;
import java.util.concurrent.CompletionStage;

public interface ZLinkBackendActorBindOperation {
    CompletionStage<Void> submit(Duration timeout);
}
