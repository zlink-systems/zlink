package systems.zlink.framework;

import java.util.concurrent.CompletionStage;

@FunctionalInterface
public interface ZLinkHandlerFilterNext<T> {
    CompletionStage<T> invoke();
}
