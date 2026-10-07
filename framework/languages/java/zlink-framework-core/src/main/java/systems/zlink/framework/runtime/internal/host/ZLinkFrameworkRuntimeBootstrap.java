package systems.zlink.framework.runtime.internal.host;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;

import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

/** Starts the runtime for the Framework-owned Spring host. */
public final class ZLinkFrameworkRuntimeBootstrap {
    private static final MethodHandle START = findStart();
    private static final MethodHandle PREPARE =
            findHostMethod(
                    "prepareHost",
                    ZLinkFrameworkRuntime.class,
                    DefaultZLinkFrameworkOptions.class,
                    ZLinkRuntimeEventDispatcher.class);
    private static final MethodHandle START_PREPARED =
            findHostMethod(
                    "startPreparedHost",
                    void.class,
                    ZLinkFrameworkRuntime.class,
                    DefaultZLinkFrameworkOptions.class,
                    ZLinkBackendAdapterProvider.class,
                    ZLinkHandlerActivator.class);

    private ZLinkFrameworkRuntimeBootstrap() {}

    public static ZLinkFrameworkRuntime start(
            DefaultZLinkFrameworkOptions options,
            ZLinkBackendAdapterProvider backendProvider,
            ZLinkHandlerActivator handlerActivator,
            ZLinkRuntimeEventDispatcher eventDispatcher) {
        try {
            return (ZLinkFrameworkRuntime)
                    START.invokeExact(options, backendProvider, handlerActivator, eventDispatcher);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Framework runtime bootstrap failed", failure);
        }
    }

    public static ZLinkFrameworkRuntime prepare(
            DefaultZLinkFrameworkOptions options, ZLinkRuntimeEventDispatcher events) {
        try {
            return (ZLinkFrameworkRuntime) PREPARE.invokeExact(options, events);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Framework runtime preparation failed", failure);
        }
    }

    public static void startPrepared(
            ZLinkFrameworkRuntime runtime,
            DefaultZLinkFrameworkOptions options,
            ZLinkBackendAdapterProvider backend,
            ZLinkHandlerActivator activator) {
        try {
            START_PREPARED.invokeExact(runtime, options, backend, activator);
        } catch (RuntimeException | Error failure) {
            throw failure;
        } catch (Throwable failure) {
            throw new IllegalStateException("Framework runtime bootstrap failed", failure);
        }
    }

    private static MethodHandle findHostMethod(
            String name, Class<?> result, Class<?>... parameters) {
        try {
            return MethodHandles.privateLookupIn(
                            ZLinkFrameworkRuntime.class, MethodHandles.lookup())
                    .findStatic(
                            ZLinkFrameworkRuntime.class,
                            name,
                            MethodType.methodType(result, parameters));
        } catch (ReflectiveOperationException failure) {
            throw new ExceptionInInitializerError(failure);
        }
    }

    private static MethodHandle findStart() {
        return findHostMethod(
                "startHost",
                ZLinkFrameworkRuntime.class,
                DefaultZLinkFrameworkOptions.class,
                ZLinkBackendAdapterProvider.class,
                ZLinkHandlerActivator.class,
                ZLinkRuntimeEventDispatcher.class);
    }
}
