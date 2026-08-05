package systems.zlink.framework.runtime.actors;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Predicate;
import java.util.function.Supplier;

/**
 * Internal bridge that binds User/Entry Spot and timer handler terminals to
 * deferred Actor membership registrations.
 */
public final class ZLinkDeferredActorJoinHandlerScope {
    private ZLinkDeferredActorJoinHandlerScope() {
    }

    public static <T> CompletionStage<T> run(
        Predicate<String> actorAllowed,
        Supplier<CompletionStage<T>> operation) {
        try (ZLinkDeferredActorJoinScope.Scope scope =
                 ZLinkDeferredActorJoinScope.enterHandler(actorAllowed)) {
            return finish(scope, operation);
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public static <T> CompletionStage<T> run(
        Object runtimeScope,
        Predicate<String> actorAllowed,
        Supplier<CompletionStage<T>> operation) {
        try (ZLinkDeferredActorJoinScope.Scope scope =
                 ZLinkDeferredActorJoinScope.enterHandler(
                     Objects.requireNonNull(runtimeScope, "runtimeScope"),
                     actorAllowed)) {
            return finish(scope, operation);
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    private static <T> CompletionStage<T> finish(
        ZLinkDeferredActorJoinScope.Scope scope,
        Supplier<CompletionStage<T>> operation) {
        CompletionStage<T> handler =
            Objects.requireNonNull(operation.get(), "operation result");
        CompletableFuture<T> completion = new CompletableFuture<>();
        scope.finish(handler, null).whenComplete((ignored, error) -> {
            if (error != null) {
                completion.completeExceptionally(error);
                return;
            }
            handler.whenComplete((value, handlerError) -> {
                if (handlerError == null) {
                    completion.complete(value);
                } else {
                    completion.completeExceptionally(handlerError);
                }
            });
        });
        return completion;
    }
}
