package systems.zlink.framework.runtime.internal.host;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;

/** Starts the runtime for the Framework-owned Spring host. */
public final class ZLinkFrameworkRuntimeBootstrap {
    private static final MethodHandle START = findStart();

    private ZLinkFrameworkRuntimeBootstrap() {
    }

    public static ZLinkFrameworkRuntime start(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendProvider,
        ZLinkHandlerActivator handlerActivator,
        ZLinkRuntimeEventDispatcher eventDispatcher) {
        try {
            return (ZLinkFrameworkRuntime) START.invokeExact(
                options,
                backendProvider,
                handlerActivator,
                eventDispatcher);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException(
                "Framework runtime bootstrap failed",
                failure);
        }
    }

    private static MethodHandle findStart() {
        try {
            MethodHandles.Lookup hostLookup = MethodHandles.privateLookupIn(
                ZLinkFrameworkRuntime.class,
                MethodHandles.lookup());
            return hostLookup.findStatic(
                ZLinkFrameworkRuntime.class,
                "start",
                MethodType.methodType(
                    ZLinkFrameworkRuntime.class,
                    DefaultZLinkFrameworkOptions.class,
                    ZLinkBackendAdapterProvider.class,
                    ZLinkHandlerActivator.class,
                    ZLinkRuntimeEventDispatcher.class));
        } catch (ReflectiveOperationException failure) {
            throw new ExceptionInInitializerError(failure);
        }
    }
}
