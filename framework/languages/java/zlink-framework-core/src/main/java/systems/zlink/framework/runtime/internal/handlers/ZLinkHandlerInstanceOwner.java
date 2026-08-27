package systems.zlink.framework.runtime.internal.handlers;
import java.util.Objects;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionException;
import java.util.function.Supplier;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/**
 * Owns handler instances for one Framework lifecycle boundary.
 */
public final class ZLinkHandlerInstanceOwner implements AutoCloseable {
    private final ZLinkHandlerActivator.Activation activation;
    private final Map<Class<?>, Object> instances = new LinkedHashMap<>();
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private boolean closed;

    public ZLinkHandlerInstanceOwner(ZLinkHandlerActivator activator) {
        this.activation = Objects.requireNonNull(activator, "activator")
            .openActivation();
    }

    public Object instance(Class<?> handlerType) {
        return inStateLane(() -> {
            Objects.requireNonNull(handlerType, "handlerType");
            if (closed) {
                throw new IllegalStateException("handler instance owner is closed");
            }
            return instances.computeIfAbsent(handlerType, activation::create);
        });
    }

    @Override
    public void close() {
        List<Object> owned;
        owned = inStateLane(() -> {
            if (closed) {
                return null;
            }
            closed = true;
            List<Object> current = new ArrayList<>(instances.values());
            instances.clear();
            return current;
        });
        if (owned == null) {
            return;
        }
        RuntimeException firstFailure = null;
        for (int index = owned.size() - 1; index >= 0; index--) {
            try {
                activation.destroy(owned.get(index));
            } catch (RuntimeException failure) {
                if (firstFailure == null) {
                    firstFailure = failure;
                } else {
                    firstFailure.addSuppressed(failure);
                }
            }
        }
        try {
            activation.close();
        } catch (RuntimeException failure) {
            if (firstFailure == null) {
                firstFailure = failure;
            } else {
                firstFailure.addSuppressed(failure);
            }
        }
        if (firstFailure != null) {
            throw firstFailure;
        }
    }

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }
}
