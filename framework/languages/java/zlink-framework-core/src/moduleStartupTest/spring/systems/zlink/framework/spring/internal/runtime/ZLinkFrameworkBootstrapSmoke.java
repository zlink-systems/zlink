package systems.zlink.framework.spring.internal.runtime;

import java.time.Duration;
import java.time.Instant;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.host.ZLinkFrameworkRuntimeBootstrap;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;

public final class ZLinkFrameworkBootstrapSmoke {
    private ZLinkFrameworkBootstrapSmoke() {
    }

    public static void main(String[] arguments) throws Exception {
        try (var runtime = ZLinkFrameworkRuntimeBootstrap.start(
            new DefaultZLinkFrameworkOptions(),
            new ZLinkJavaBackendAdapterFactory(),
            ZLinkHandlerActivator.reflection(),
            new ZLinkRuntimeEventDispatcher())) {
            Instant deadline = Instant.now().plus(Duration.ofSeconds(5));
            while (runtime.status().state() == ZLinkFrameworkRuntimeState.PREPARING
                && Instant.now().isBefore(deadline)) {
                Thread.sleep(10L);
            }
            if (runtime.status().state() != ZLinkFrameworkRuntimeState.SERVING) {
                throw new IllegalStateException(
                    "modular runtime did not enter SERVING: "
                        + runtime.status().state());
            }
            System.out.println("modular-spring-bootstrap=SERVING");
        }
    }
}
