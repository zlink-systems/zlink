package systems.zlink.framework;

import java.util.concurrent.CompletionStage;

public interface ZLinkHandlerFilter {
    <T> CompletionStage<T> invoke(
        ZLinkHandlerFilterContext context,
        ZLinkHandlerFilterNext<T> next);
}
