package systems.zlink.framework.spots;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface ZLinkIoWorkerTask<T> {
    CompletionStage<T> run(ZLinkWorkerCancellation cancellation) throws Exception;
}
