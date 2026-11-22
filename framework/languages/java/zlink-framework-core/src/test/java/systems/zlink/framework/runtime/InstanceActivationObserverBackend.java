package systems.zlink.framework.runtime;

import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.service.ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Function;

/** Observes Instance Spot envelopes at the target activation validator. */
public final class InstanceActivationObserverBackend implements ZLinkBackendAdapterProvider {
    private final ZLinkJavaBackendAdapterFactory delegate = new ZLinkJavaBackendAdapterFactory();
    private final Consumer<RecoveryEnvelope> observer;

    public InstanceActivationObserverBackend(Consumer<RecoveryEnvelope> observer) {
        this.observer = observer;
    }

    public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
        return delegate.createChannelAdapter(options);
    }

    public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
        return delegate.createSpotAdapter(options);
    }

    public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
        return delegate.createStreamAdapter(options);
    }

    public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
            ZLinkBackendAdapterOptions options) {
        return delegate.createMonitoringAdapter(options);
    }

    @SuppressWarnings("unchecked")
    public ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
        var adapter = delegate.createMeshAdapter(options);
        return (context, name) -> {
            var node = adapter.createMeshNode(context, name);
            return (ZLinkInternalMeshNode)
                    Proxy.newProxyInstance(
                            ZLinkInternalMeshNode.class.getClassLoader(),
                            new Class<?>[] {ZLinkInternalMeshNode.class},
                            (proxy, method, arguments) -> {
                                if (method.getName().equals("setInstanceSpotActivationValidator")) {
                                    var original =
                                            (Function<
                                                            RecoveryEnvelope,
                                                            CompletionStage<
                                                                    ZLinkAuthorityReadResult>>)
                                                    arguments[0];
                                    arguments[0] =
                                            (Function<
                                                            RecoveryEnvelope,
                                                            CompletionStage<
                                                                    ZLinkAuthorityReadResult>>)
                                                    envelope -> {
                                                        observer.accept(envelope);
                                                        return original.apply(envelope);
                                                    };
                                }
                                try {
                                    return method.invoke(node, arguments);
                                } catch (InvocationTargetException failure) {
                                    throw failure.getCause();
                                }
                            });
        };
    }
}
