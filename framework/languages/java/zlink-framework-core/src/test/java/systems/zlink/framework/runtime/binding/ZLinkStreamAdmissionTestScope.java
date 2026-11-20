package systems.zlink.framework.runtime.binding;

import systems.zlink.runtime.framework.FrameworkStreamOperations;

import java.util.function.UnaryOperator;

/** Restores the binding's STREAM admission bridge after a controlled test boundary. */
public final class ZLinkStreamAdmissionTestScope implements AutoCloseable {
    private final FrameworkStreamOperations.Access previous;

    public ZLinkStreamAdmissionTestScope(UnaryOperator<FrameworkStreamOperations.Access> wrapper)
            throws ReflectiveOperationException {
        var access = FrameworkStreamOperations.class.getDeclaredField("access");
        access.setAccessible(true);
        previous = (FrameworkStreamOperations.Access) access.get(null);
        FrameworkStreamOperations.register(wrapper.apply(previous));
    }

    public static systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket
            createStreamSocket(systems.zlink.contracts.core.Context context) {
        return new ZLinkJavaStreamSocket(
                context.createStreamSocket(),
                null,
                new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane(
                        Runnable::run));
    }

    @Override
    public void close() {
        FrameworkStreamOperations.register(previous);
    }
}
