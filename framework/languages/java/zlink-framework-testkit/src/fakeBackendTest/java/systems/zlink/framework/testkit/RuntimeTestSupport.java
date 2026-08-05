package systems.zlink.framework.testkit;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
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

    static ZLinkFrameworkRuntime newFrameworkRuntime(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory) {
        return invoke(() -> {
            Constructor<ZLinkFrameworkRuntime> constructor =
                ZLinkFrameworkRuntime.class.getDeclaredConstructor(
                    DefaultZLinkFrameworkOptions.class,
                    ZLinkBackendAdapterProvider.class,
                    ZLinkMessageSerializer.class,
                    ZLinkHandlerActivator.class);
            constructor.setAccessible(true);
            return constructor.newInstance(options, backendFactory, serializer, handlerFactory);
        });
    }

    static void awaitClosed(FakeZLinkBackendAdapterFactory backendFactory) {
        awaitClosed(backendFactory, 1);
    }

    static void awaitClosed(FakeZLinkBackendAdapterFactory backendFactory, long contextCount) {
        long deadline = System.nanoTime() + java.time.Duration.ofSeconds(2).toNanos();
        while (System.nanoTime() < deadline) {
            if (backendFactory.calls().stream().filter("close.context"::equals).count() >= contextCount) {
                return;
            }
            Thread.onSpinWait();
        }
        throw new AssertionError("runtime close did not complete: " + backendFactory.calls());
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
