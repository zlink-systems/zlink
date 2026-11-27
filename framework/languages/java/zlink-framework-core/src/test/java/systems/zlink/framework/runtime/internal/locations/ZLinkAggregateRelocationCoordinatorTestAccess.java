package systems.zlink.framework.runtime.internal.locations;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletionStage;

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
            ZLinkAggregateRelocationCoordinator coordinator, CommitInterceptor interceptor) {
        InvocationHandler handler = Proxy.getInvocationHandler(coordinator.authorityStore);
        ((InterceptingHandler) handler).interceptor =
                Objects.requireNonNull(interceptor, "interceptor");
    }

    @FunctionalInterface
    public interface CommitInterceptor {
        CompletionStage<ZLinkAggregateCommitResult> commit(
                ZLinkLocationRepository repository,
                ZLinkAggregateFence fence,
                List<Long> acceptedRequests,
                ZLinkStoreCancellation cancellation);
    }

    private static final class InterceptingHandler implements InvocationHandler {
        private final ZLinkLocationRepository delegate;
        private volatile CommitInterceptor interceptor;

        private InterceptingHandler(ZLinkLocationRepository delegate) {
            this.delegate = Objects.requireNonNull(delegate, "delegate");
        }

        @Override
        @SuppressWarnings("unchecked")
        public Object invoke(Object proxy, Method method, Object[] arguments) throws Throwable {
            var current = interceptor;
            if (current != null && method.getName().equals("commitAggregate")) {
                return current.commit(
                        delegate,
                        (ZLinkAggregateFence) arguments[0],
                        (List<Long>) arguments[1],
                        (ZLinkStoreCancellation) arguments[2]);
            }
            try {
                return method.invoke(delegate, arguments);
            } catch (InvocationTargetException failure) {
                throw failure.getCause();
            }
        }
    }
}
