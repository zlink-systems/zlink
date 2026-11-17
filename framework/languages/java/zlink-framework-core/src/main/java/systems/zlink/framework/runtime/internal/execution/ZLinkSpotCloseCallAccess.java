package systems.zlink.framework.runtime.internal.execution;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;

import java.util.Objects;

/** Internal bridge for Spot Close admission checks owned by the execution queue. */
public final class ZLinkSpotCloseCallAccess {
    private static volatile SpotCloseCallAccess access;

    private ZLinkSpotCloseCallAccess() {}

    /** Implemented by the execution queue, which owns the package-private check. */
    public interface SpotCloseCallAccess {
        void ensureAllowed(ZLinkSerialExecutionQueue queue);
    }

    /** Registers the queue implementation during its class initialization. */
    public static synchronized void registerAccess(SpotCloseCallAccess implementation) {
        if (access != null) {
            throw new IllegalStateException("Spot Close access is already registered");
        }
        access = Objects.requireNonNull(implementation, "implementation");
    }

    /** Enforces the queue's Spot Close execution gate check. */
    public static void ensureAllowed(ZLinkSerialExecutionQueue queue) {
        SpotCloseCallAccess selected = access;
        if (selected == null) {
            // The queue instance implies its class initializer registered this bridge.
            throw new IllegalStateException("Spot Close access is not registered");
        }
        selected.ensureAllowed(Objects.requireNonNull(queue, "queue"));
    }
}
