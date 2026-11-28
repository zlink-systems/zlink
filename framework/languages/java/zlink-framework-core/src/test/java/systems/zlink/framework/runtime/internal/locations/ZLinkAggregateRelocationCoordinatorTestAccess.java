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
        handler(coordinator).commit = Objects.requireNonNull(interceptor, "interceptor");
    }

    public static void interceptRead(
            ZLinkAggregateRelocationCoordinator coordinator, ReadInterceptor interceptor) {
        handler(coordinator).read = Objects.requireNonNull(interceptor, "interceptor");
    }

    private static InterceptingHandler handler(ZLinkAggregateRelocationCoordinator coordinator) {
        return (InterceptingHandler) Proxy.getInvocationHandler(coordinator.authorityStore);
    }

    @FunctionalInterface
    public interface CommitInterceptor {
        CompletionStage<ZLinkAggregateCommitResult> commit(
                ZLinkLocationRepository repository,
                ZLinkAggregateFence fence,
                List<ZLinkRelocationReplyRow> acceptedReplies,
                ZLinkStoreCancellation cancellation);
    }

    @FunctionalInterface
    public interface ReadInterceptor {
        CompletionStage<ZLinkAuthorityReadResult> read(
                ZLinkLocationRepository repository,
                String key,
                ZLinkStoreCancellation cancellation);
    }

    private static final class InterceptingHandler implements InvocationHandler {
        private final ZLinkLocationRepository delegate;
        private volatile CommitInterceptor commit;
        private volatile ReadInterceptor read;

        private InterceptingHandler(ZLinkLocationRepository delegate) {
            this.delegate = Objects.requireNonNull(delegate, "delegate");
        }

        @Override
        @SuppressWarnings("unchecked")
        public Object invoke(Object proxy, Method method, Object[] arguments) throws Throwable {
            var current = commit;
            if (current != null && method.getName().equals("commitAggregate")) {
                return current.commit(
                        delegate,
                        (ZLinkAggregateFence) arguments[0],
                        (List<ZLinkRelocationReplyRow>) arguments[1],
                        (ZLinkStoreCancellation) arguments[2]);
            }
            var readInterceptor = read;
            if (readInterceptor != null && method.getName().equals("read")) {
                return readInterceptor.read(
                        delegate, (String) arguments[0], (ZLinkStoreCancellation) arguments[1]);
            }
            try {
                return method.invoke(delegate, arguments);
            } catch (InvocationTargetException failure) {
                throw failure.getCause();
            }
        }
    }
}
