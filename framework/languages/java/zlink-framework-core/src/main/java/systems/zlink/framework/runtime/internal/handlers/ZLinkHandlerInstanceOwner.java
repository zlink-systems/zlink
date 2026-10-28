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
                    if (activation == null) activation = openActivation(activator);
                    return instances.computeIfAbsent(handlerType, activation::create);
                });
    }

    public static ZLinkHandlerActivator.Activation openActivation(ZLinkHandlerActivator activator) {
        return activator.openActivation();
    }

    /** Creates a single dispatch handler and releases its scope if creation fails. */
    public static Object createSingle(
            ZLinkHandlerActivator.Activation activation, Class<?> handlerType) {
        try {
            return activation.create(handlerType);
        } catch (RuntimeException | Error failure) {
            RuntimeException cleanupFailure;
            try {
                cleanupFailure = release(activation, null, true);
            } catch (Error closeFailure) {
                failure.addSuppressed(closeFailure);
                throw failure;
            }
            if (cleanupFailure != null) failure.addSuppressed(cleanupFailure);
            throw failure;
        }
    }

    /** Releases a single dispatch handler once; cleanup failures are terminal. */
    public static void closeSingle(ZLinkHandlerActivator.Activation activation, Object handler) {
        RuntimeException failure = release(activation, handler, true);
        if (failure != null) throw failure;
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
                        Map.Entry<Class<?>, Object> entry = releases.get(index);
                        RuntimeException failure = release(activation, entry.getValue(), false);
                        if (failure == null) instances.remove(entry.getKey());
                        else firstFailure = addFailure(firstFailure, failure);
                    }
                    if (activation != null && !activationClosed) {
                        RuntimeException failure = release(activation, null, true);
                        if (failure == null) activationClosed = true;
                        else firstFailure = addFailure(firstFailure, failure);
                    }
                    if (firstFailure != null) throw firstFailure;
                    return null;
                });
    }

    /** The one destroy/close success decision for both ownership shapes. */
    private static RuntimeException release(
            ZLinkHandlerActivator.Activation activation, Object handler, boolean closeScope) {
        Throwable failure = null;
        if (handler != null) {
            try {
                activation.destroy(handler);
            } catch (RuntimeException | Error destroyFailure) {
                failure = destroyFailure;
            }
        }
        if (closeScope) {
            try {
                activation.close();
            } catch (RuntimeException | Error closeFailure) {
                failure = addFailure(failure, closeFailure);
            }
        }
        if (failure instanceof Error error) throw error;
        return (RuntimeException) failure;
    }

    private static <T extends Throwable> T addFailure(T first, T failure) {
        if (first == null) return failure;
        first.addSuppressed(failure);
        return first;
    }

    private <T> T inStateLane(Supplier<T> work) {
        return stateLane.awaitTurn(work);
    }
}
