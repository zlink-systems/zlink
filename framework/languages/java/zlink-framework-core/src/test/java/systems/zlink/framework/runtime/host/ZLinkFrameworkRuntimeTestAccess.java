package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;

/** Test-only access to the package-owned runtime bootstrap. */
public final class ZLinkFrameworkRuntimeTestAccess {
    private ZLinkFrameworkRuntimeTestAccess() {
    }

    public static ZLinkFrameworkRuntime start(
        DefaultZLinkFrameworkOptions options) {
        return ZLinkFrameworkRuntime.start(
            options,
            new ZLinkJavaBackendAdapterFactory());
    }

    public static ZLinkFrameworkRuntime start(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendProvider) {
        return ZLinkFrameworkRuntime.start(options, backendProvider);
    }

    public static ZLinkFrameworkRuntime start(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendProvider,
        ZLinkHandlerActivator handlerActivator,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        return ZLinkFrameworkRuntime.start(
            options,
            backendProvider,
            handlerActivator,
            eventDispatcher);
    }
}
