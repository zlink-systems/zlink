package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkTrackingHandlerActivator implements ZLinkHandlerActivator {
    private final ZLinkHandlerActivator delegate = ZLinkHandlerActivator.reflection();
    final AtomicInteger opened = new AtomicInteger();
    final AtomicInteger closed = new AtomicInteger();

    @Override
    public Object create(Class<?> type) {
        return delegate.create(type);
    }

    @Override
    public Activation openActivation() {
        opened.incrementAndGet();
        var activation = delegate.openActivation();
        return new Activation() {
            @Override
            public Object create(Class<?> type) {
                return activation.create(type);
            }

            @Override
            public void destroy(Object value) {
                activation.destroy(value);
            }

            @Override
            public void close() {
                activation.close();
                closed.incrementAndGet();
            }
        };
    }
}
