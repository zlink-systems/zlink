package systems.zlink.framework.testkit;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTestkitRuntimeAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;

import java.time.Duration;

final class RuntimeTestSupport {
    private RuntimeTestSupport() {}

    static ZLinkFrameworkRuntime startFramework(
            DefaultZLinkFrameworkOptions options, ZLinkBackendAdapterProvider backendFactory) {
        return ZLinkFrameworkTestkitRuntimeAccess.start(options, backendFactory);
    }

    static void awaitClosed(FakeZLinkBackendAdapterFactory backendFactory) {
        awaitClosed(backendFactory, 1);
    }

    static void awaitClosed(FakeZLinkBackendAdapterFactory backendFactory, long contextCount) {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (System.nanoTime() < deadline) {
            if (backendFactory.calls().stream().filter("close.context"::equals).count()
                    >= contextCount) {
                return;
            }
            Thread.onSpinWait();
        }
        throw new AssertionError("runtime close did not complete: " + backendFactory.calls());
    }
}
