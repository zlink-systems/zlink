package systems.zlink.framework.runtime;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

final class RuntimeTestSupport {
    private RuntimeTestSupport() {
    }

    static ZLinkFrameworkRuntime startFramework(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendFactory) {
        return invoke(() -> {
            Method start = ZLinkFrameworkRuntime.class.getDeclaredMethod(
                "start",
                DefaultZLinkFrameworkOptions.class,
                ZLinkBackendAdapterProvider.class);
            start.setAccessible(true);
            return (ZLinkFrameworkRuntime) start.invoke(null, options, backendFactory);
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
