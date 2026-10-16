package systems.zlink.framework.runtime.internal.handlers;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.function.Supplier;

/** Owns handler instances for one Framework lifecycle boundary. */
public final class ZLinkHandlerInstanceOwner implements AutoCloseable {
    private final ZLinkHandlerActivator activator;
    private ZLinkHandlerActivator.Activation activation;
    private final Map<Class<?>, Object> instances = new LinkedHashMap<>();
    // Keep synchronous activation on the caller's execution resource; the lane
    // still owns FIFO and non-reentrant access to the scope's state.
    private final ZLinkStateLane stateLane = new ZLinkStateLane(Runnable::run);
    private boolean closed;
    private boolean activationClosed;

    public ZLinkHandlerInstanceOwner(ZLinkHandlerActivator activator) {
        this.activator = Objects.requireNonNull(activator, "activator");
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
                    if (activation == null) activation = activator.openActivation();
                    return instances.computeIfAbsent(handlerType, activation::create);
                });
    }

    /**
     * Releases the owned handler instances and the activation scope. A release that fails stays
     * owned, so a later call retries only the failed releases; completed releases are not repeated.
     */
    @Override
    public void close() {
        inStateLane(
                () -> {
                    closed = true;
                    List<Map.Entry<Class<?>, Object>> releases =
                            instances.isEmpty() ? List.of() : new ArrayList<>(instances.entrySet());
                    RuntimeException firstFailure = null;
                    for (int index = releases.size() - 1; index >= 0; index--) {
                        Map.Entry<Class<?>, Object> release = releases.get(index);
                        try {
                            activation.destroy(release.getValue());
                            instances.remove(release.getKey());
                        } catch (RuntimeException failure) {
                            firstFailure = addFailure(firstFailure, failure);
                        }
                    }
                    if (activation != null && !activationClosed) {
                        try {
                            activation.close();
                            activationClosed = true;
                        } catch (RuntimeException failure) {
                            firstFailure = addFailure(firstFailure, failure);
                        }
                    }
                    if (firstFailure != null) throw firstFailure;
                    return null;
                });
    }

    private static RuntimeException addFailure(RuntimeException first, RuntimeException failure) {
        if (first == null) {
            return failure;
        }
        first.addSuppressed(failure);
        return first;
    }

    private <T> T inStateLane(Supplier<T> work) {
        return stateLane.awaitTurn(work);
    }
}
