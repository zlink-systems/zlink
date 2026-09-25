package systems.zlink.framework.runtime;

import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

final class RuntimeTestSupport {
    private RuntimeTestSupport() {}

    static ZLinkFrameworkRuntime startFramework(
            DefaultZLinkFrameworkOptions options, ZLinkBackendAdapterProvider backendFactory) {
        return invoke(
                () -> {
                    Method start =
                            ZLinkFrameworkRuntime.class.getDeclaredMethod(
                                    "start",
                                    DefaultZLinkFrameworkOptions.class,
                                    ZLinkBackendAdapterProvider.class);
                    start.setAccessible(true);
                    return (ZLinkFrameworkRuntime) start.invoke(null, options, backendFactory);
                });
    }

    /**
     * Reads the endpoint a legacy route channel ROUTER actually bound, straight from the router
     * that owns it. Legacy route channels are not public listeners, so listenerStatus does not
     * report them.
     */
    static String legacyRouteBoundEndpoint(ZLinkFrameworkRuntime runtime, String channelName) {
        return invoke(
                () -> {
                    Method router =
                            ZLinkChannelRuntime.class.getDeclaredMethod(
                                    "requireRouteRouter", String.class);
                    router.setAccessible(true);
                    return ((ZLinkBackendRouterSocket) router.invoke(runtime.client(), channelName))
                            .lastEndpoint();
                });
    }

    private static <T> T invoke(ReflectiveCall<T> call) {
        try {
            return call.invoke();
        } catch (InvocationTargetException error) {
            Throwable cause = error.getCause();
            if (cause instanceof RuntimeException runtimeException) {
                throw runtimeException;
            }
            if (cause instanceof Error fatal) {
                throw fatal;
            }
            throw new IllegalStateException(cause);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException(error);
        }
    }

    private interface ReflectiveCall<T> {
        T invoke() throws ReflectiveOperationException;
    }
}
