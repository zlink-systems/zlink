package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;

import io.micrometer.core.instrument.simple.SimpleMeterRegistry;

import org.junit.jupiter.api.Test;
import org.springframework.context.SmartLifecycle;
import org.springframework.context.support.GenericApplicationContext;

import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkMetricsLifecycleTest {
    @Test
    void providerRemainsAttachedThroughFrameworkShutdown() {
        var registry = new SimpleMeterRegistry();
        var observed = new ArrayList<String>();
        var state = new AtomicReference<>("preparing");
        try (var context = new GenericApplicationContext()) {
            context.registerBean(
                    "metricsLifecycle",
                    ZLinkMetricsLifecycle.class,
                    () -> new ZLinkMetricsLifecycle(registry, List.of()));
            context.registerBean(
                    "frameworkLifecycle",
                    SmartLifecycle.class,
                    () ->
                            new SmartLifecycle() {
                                private boolean running;
                                private AutoCloseable registration;

                                @Override
                                public int getPhase() {
                                    return ZLinkFrameworkLifecycle.PHASE;
                                }

                                @Override
                                public void start() {
                                    registration =
                                            ZLinkRuntimeMetrics.registerHostState(state::get);
                                    state.set("serving");
                                    ZLinkRuntimeMetrics.publishHostState();
                                    running = true;
                                }

                                @Override
                                public void stop() {
                                    for (String terminalState : List.of("draining", "stopped")) {
                                        state.set(terminalState);
                                        ZLinkRuntimeMetrics.publishHostState();
                                        observed.add(
                                                registry.get("zlink.host.state")
                                                        .gauge()
                                                        .getId()
                                                        .getTag("state"));
                                    }
                                    try {
                                        registration.close();
                                    } catch (Exception failure) {
                                        throw new IllegalStateException(failure);
                                    }
                                    running = false;
                                }

                                @Override
                                public boolean isRunning() {
                                    return running;
                                }
                            });
            context.refresh();
        } finally {
            registry.close();
        }
        assertEquals(List.of("draining", "stopped"), observed);
    }
}
