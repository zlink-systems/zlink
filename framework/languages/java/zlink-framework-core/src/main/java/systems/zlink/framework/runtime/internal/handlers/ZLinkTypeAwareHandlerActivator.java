package systems.zlink.framework.runtime.internal.handlers;

/** Provides shared stateless activation strategies when the registration scope allows reuse. */
public interface ZLinkTypeAwareHandlerActivator {
    /**
     * Returns an immutable strategy that still creates a fresh handler for each dispatch, or {@code
     * null} when the existing per-dispatch activation path must be used.
     */
    ZLinkHandlerActivator.Activation reusableActivation(
            Class<?> handlerType, RuntimeServiceLookup runtimeServices);

    @FunctionalInterface
    interface RuntimeServiceLookup {
        RuntimeServiceLookup NONE = ignored -> false;

        boolean contains(Class<?> serviceType);
    }
}
