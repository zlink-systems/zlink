package systems.zlink.framework.runtime.internal.handlers;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.util.function.Supplier;

/** Owns handler instances for one Framework lifecycle boundary. */
public final class ZLinkHandlerInstanceOwner implements AutoCloseable {
    private final ZLinkHandlerActivator activator;
    private final ZLinkHandlerActivator.Activation activation;
    private final Map<Class<?>, Object> instances = new LinkedHashMap<>();
    // Keep synchronous activation on the caller's execution resource; the lane
    // still owns FIFO and non-reentrant access to the scope's state.
    private final ZLinkStateLane stateLane = new ZLinkStateLane(Runnable::run);
    private boolean closed;
    private boolean activationClosed;

    public ZLinkHandlerInstanceOwner(ZLinkHandlerActivator activator) {
        this.activator = Objects.requireNonNull(activator, "activator");
        this.activation = this.activator.openActivation();
    }

    public void prepare(Class<?> handlerType) {
        activator.prepare(handlerType);
    }

    public Object instance(Class<?> handlerType) {
        return inStateLane(
                () -> {
                    Objects.requireNonNull(handlerType, "handlerType");
                    if (closed) {
                        throw new IllegalStateException("handler instance owner is closed");
                    }
                    return instances.computeIfAbsent(handlerType, activation::create);
                });
    }

    /**
     * Releases the owned handler instances and the activation scope. A release that fails stays
     * owned, so a later call retries only the failed releases; completed releases are not repeated.
     */
    @Override
    public void close() {
        Map<Class<?>, Object> owned =
                inStateLane(
                        () -> {
                            closed = true;
                            Map<Class<?>, Object> current = new LinkedHashMap<>(instances);
                            instances.clear();
                            return current;
                        });
        List<Map.Entry<Class<?>, Object>> releases = new ArrayList<>(owned.entrySet());
        Map<Class<?>, Object> failed = new LinkedHashMap<>();
        RuntimeException firstFailure = null;
        for (int index = releases.size() - 1; index >= 0; index--) {
            Map.Entry<Class<?>, Object> release = releases.get(index);
            try {
                activation.destroy(release.getValue());
            } catch (RuntimeException failure) {
                failed.put(release.getKey(), release.getValue());
                firstFailure = addFailure(firstFailure, failure);
            }
        }
        boolean closeActivation = inStateLane(() -> !activationClosed);
        if (closeActivation) {
            try {
                activation.close();
                inStateLane(() -> activationClosed = true);
            } catch (RuntimeException failure) {
                firstFailure = addFailure(firstFailure, failure);
            }
        }
        if (firstFailure != null) {
            inStateLane(
                    () -> {
                        instances.putAll(failed);
                        return null;
                    });
            throw firstFailure;
        }
    }

    private static RuntimeException addFailure(RuntimeException first, RuntimeException failure) {
        if (first == null) {
            return failure;
        }
        first.addSuppressed(failure);
        return first;
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
