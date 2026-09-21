package systems.zlink.framework.spring;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;

/** Observes the existing fake monitor through the runtime's pull contract. */
final class MonitorRecvProbe {
    private final CompletableFuture<Void> received = new CompletableFuture<>();
    private final List<String> calls = new CopyOnWriteArrayList<>();

    ZLinkBackendAdapterProvider observe(FakeZLinkBackendAdapterFactory backend) {
        return (ZLinkBackendAdapterProvider)
                Proxy.newProxyInstance(
                        ZLinkBackendAdapterProvider.class.getClassLoader(),
                        new Class<?>[] {ZLinkBackendAdapterProvider.class},
                        (proxy, method, arguments) -> {
                            Object result;
                            try {
                                result = method.invoke(backend, arguments);
                            } catch (InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                            if (!(result instanceof ZLinkMonitoringBackendAdapter monitoring)) {
                                return result;
                            }
                            return (ZLinkMonitoringBackendAdapter)
                                    socket -> {
                                        ZLinkBackendSocketMonitor monitor =
                                                monitoring.openSocketMonitor(socket);
                                        return new ZLinkBackendSocketMonitor() {
                                            @Override
                                            public boolean waitForReadable(Duration timeout) {
                                                calls.add("socketMonitor.waitForReadable");
                                                received.complete(null);
                                                return monitor.waitForReadable(timeout);
                                            }

                                            @Override
                                            public ZLinkBackendSocketMonitorEvent recvDontWait() {
                                                return monitor.recvDontWait();
                                            }

                                            @Override
                                            public boolean isClosed() {
                                                return monitor.isClosed();
                                            }

                                            @Override
                                            public String name() {
                                                return monitor.name();
                                            }

                                            @Override
                                            public void close() {
                                                monitor.close();
                                                calls.add("close.socketMonitor");
                                            }
                                        };
                                    };
                        });
    }

    void awaitRecv() throws Exception {
        received.get(1, TimeUnit.SECONDS);
    }

    List<String> calls() {
        return List.copyOf(calls);
    }
}
