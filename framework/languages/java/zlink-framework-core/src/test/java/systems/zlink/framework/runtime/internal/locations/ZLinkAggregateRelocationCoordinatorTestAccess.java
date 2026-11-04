package systems.zlink.framework.runtime.internal.locations;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Objects;
import java.util.concurrent.CompletionStage;
import java.util.function.BiFunction;

public final class ZLinkAggregateRelocationCoordinatorTestAccess {
    private ZLinkAggregateRelocationCoordinatorTestAccess() {}

    public static ZLinkLocationRepository authorityStore(
            ZLinkAggregateRelocationCoordinator coordinator) {
        return coordinator.authorityStore;
    }

    public static ZLinkLocationRepository interceptingStore(
            ZLinkLocationRepository authorityStore) {
        return (ZLinkLocationRepository)
                Proxy.newProxyInstance(
                        ZLinkLocationRepository.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        new InterceptingHandler(authorityStore));
    }

    public static void interceptCommit(
            ZLinkAggregateRelocationCoordinator coordinator,
            BiFunction<ZLinkLocationRepository, Object[], CompletionStage<?>> interceptor) {
        InvocationHandler handler = Proxy.getInvocationHandler(coordinator.authorityStore);
        ((InterceptingHandler) handler).interceptor =
                Objects.requireNonNull(interceptor, "interceptor");
    }

    private static final class InterceptingHandler implements InvocationHandler {
        private final ZLinkLocationRepository delegate;
        private volatile BiFunction<ZLinkLocationRepository, Object[], CompletionStage<?>>
                interceptor;

        private InterceptingHandler(ZLinkLocationRepository delegate) {
            this.delegate = Objects.requireNonNull(delegate, "delegate");
        }

        @Override
        public Object invoke(Object proxy, Method method, Object[] arguments) throws Throwable {
            var current = interceptor;
            if (current != null && method.getName().equals("commitAggregate")) {
                return current.apply(delegate, arguments);
            }
            try {
                return method.invoke(delegate, arguments);
            } catch (InvocationTargetException failure) {
                throw failure.getCause();
            }
        }
    }
}
