package systems.zlink.framework.runtime.internal.handlers;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.ArrayList;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletionException;
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
            PendingRelease pending;
            try {
                pending = release(activation, null, true);
            } catch (Error closeFailure) {
                failure.addSuppressed(closeFailure);
                throw failure;
            }
            if (pending == null) throw failure;
            failure.addSuppressed(pending.failure());
            if (failure instanceof Error error) throw error;
            throw new PendingRelease(
                    activation, null, pending.closeScope, (RuntimeException) failure);
        }
    }

    /** Successful single-instance release needs no ownership record. */
    public static void closeSingle(ZLinkHandlerActivator.Activation activation, Object handler) {
        PendingRelease pending = release(activation, handler, true);
        if (pending != null) throw pending;
    }

    /**
     * Finds failed cleanup ownership in a dispatch failure, including when the handler also failed.
     */
    public static PendingRelease pendingRelease(Throwable failure) {
        return findPendingRelease(failure, Collections.newSetFromMap(new IdentityHashMap<>()));
    }

    private static PendingRelease findPendingRelease(Throwable failure, Set<Throwable> seen) {
        if (failure == null || !seen.add(failure)) return null;
        if (failure instanceof PendingRelease pending) return pending;
        PendingRelease pending = findPendingRelease(failure.getCause(), seen);
        if (pending != null) return pending;
        for (Throwable suppressed : failure.getSuppressed()) {
            pending = findPendingRelease(suppressed, seen);
            if (pending != null) return pending;
        }
        return null;
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
                        PendingRelease pending = release(activation, entry.getValue(), false);
                        if (pending == null) instances.remove(entry.getKey());
                        else firstFailure = addFailure(firstFailure, pending.failure());
                    }
                    if (activation != null && !activationClosed) {
                        PendingRelease pending = release(activation, null, true);
                        if (pending == null) activationClosed = true;
                        else firstFailure = addFailure(firstFailure, pending.failure());
                    }
                    if (firstFailure != null) throw firstFailure;
                    return null;
                });
    }

    /** The one destroy/close success decision for both ownership shapes. */
    private static PendingRelease release(
            ZLinkHandlerActivator.Activation activation, Object handler, boolean closeScope) {
        Throwable failure = null;
        if (handler != null) {
            try {
                activation.destroy(handler);
                handler = null;
            } catch (RuntimeException | Error destroyFailure) {
                failure = destroyFailure;
            }
        }
        if (closeScope) {
            try {
                activation.close();
                closeScope = false;
            } catch (RuntimeException | Error closeFailure) {
                failure = addFailure(failure, closeFailure);
            }
        }
        if (failure instanceof Error error) throw error;
        return failure == null
                ? null
                : new PendingRelease(activation, handler, closeScope, (RuntimeException) failure);
    }

    /** A failed release is its own retry handle; successful dispatches never create one. */
    public static final class PendingRelease extends CompletionException implements AutoCloseable {
        private final ZLinkHandlerActivator.Activation activation;
        private Object handler;
        private boolean closeScope;

        private PendingRelease(
                ZLinkHandlerActivator.Activation activation,
                Object handler,
                boolean closeScope,
                RuntimeException failure) {
            super(failure);
            this.activation = activation;
            this.handler = handler;
            this.closeScope = closeScope;
        }

        private RuntimeException failure() {
            return (RuntimeException) getCause();
        }

        @Override
        public Throwable fillInStackTrace() {
            return this;
        }

        @Override
        public void close() {
            PendingRelease pending = release(activation, handler, closeScope);
            handler = pending == null ? null : pending.handler;
            closeScope = pending != null && pending.closeScope;
            if (pending != null) {
                if (pending.failure() != failure()) addSuppressed(pending.failure());
                throw this;
            }
        }
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
