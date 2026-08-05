package systems.zlink.framework.spots;

import java.util.concurrent.CompletionStage;

public interface ZLinkTimer extends AutoCloseable {
    boolean isDisposed();

    CompletionStage<Void> cancel();

    @Override
    void close();
}
