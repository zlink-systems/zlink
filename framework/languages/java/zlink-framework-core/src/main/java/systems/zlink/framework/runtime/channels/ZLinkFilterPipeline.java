package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;

import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkHandlerFilterContext;
import systems.zlink.framework.ZLinkHandlerFilterNext;

final class ZLinkFilterPipeline {
    record Result<T>(boolean handlerInvoked, T value) {
    }

    private ZLinkFilterPipeline() {
    }

    public static <T> CompletionStage<Result<T>> invoke(
        List<Class<? extends ZLinkHandlerFilter>> filterTypes,
        ZLinkHandlerInstanceOwner handlers,
        ZLinkHandlerFilterContext context,
        Supplier<CompletionStage<T>> terminal) {
        AtomicReference<CompletionStage<T>> terminalStage =
            new AtomicReference<>();
        ZLinkHandlerFilterNext<T> next = invokeAtMostOnce(() -> {
            CompletionStage<T> stage =
                Objects.requireNonNull(terminal.get(), "handler stage");
            terminalStage.set(stage);
            return stage;
        });
        for (int index = filterTypes.size() - 1; index >= 0; index--) {
            Class<? extends ZLinkHandlerFilter> filterType = filterTypes.get(index);
            ZLinkHandlerFilterNext<T> currentNext = invokeAtMostOnce(next);
            next = () -> {
                @SuppressWarnings("unchecked")
                ZLinkHandlerFilter filter =
                    (ZLinkHandlerFilter) handlers.instance(filterType);
                return Objects.requireNonNull(
                    filter.invoke(context, currentNext),
                    "filter stage");
            };
        }
        return next.invoke().thenCompose(ignored -> {
            CompletionStage<T> invoked = terminalStage.get();
            if (invoked == null) {
                return java.util.concurrent.CompletableFuture.completedFuture(
                    new Result<>(false, null));
            }
            return invoked.thenApply(value -> new Result<>(true, value));
        });
    }

    private static <T> ZLinkHandlerFilterNext<T> invokeAtMostOnce(
        ZLinkHandlerFilterNext<T> next) {
        AtomicBoolean invoked = new AtomicBoolean();
        return () -> {
            if (!invoked.compareAndSet(false, true)) {
                throw new IllegalStateException(
                    "A handler filter cannot invoke next more than once.");
            }
            return next.invoke();
        };
    }
}
