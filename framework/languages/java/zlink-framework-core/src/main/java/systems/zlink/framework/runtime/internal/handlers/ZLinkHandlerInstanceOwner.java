package systems.zlink.framework.runtime.internal.handlers;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Owns handler instances for one Framework lifecycle boundary.
 */
public final class ZLinkHandlerInstanceOwner implements AutoCloseable {
    private final ZLinkHandlerActivator.Activation activation;
    private final Map<Class<?>, Object> instances = new LinkedHashMap<>();
    private boolean closed;

    public ZLinkHandlerInstanceOwner(ZLinkHandlerActivator activator) {
        this.activation = java.util.Objects.requireNonNull(activator, "activator")
            .openActivation();
    }

    public synchronized Object instance(Class<?> handlerType) {
        java.util.Objects.requireNonNull(handlerType, "handlerType");
        if (closed) {
            throw new IllegalStateException("handler instance owner is closed");
        }
        return instances.computeIfAbsent(handlerType, activation::create);
    }

    @Override
    public void close() {
        List<Object> owned;
        synchronized (this) {
            if (closed) {
                return;
            }
            closed = true;
            owned = new ArrayList<>(instances.values());
            instances.clear();
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
}
