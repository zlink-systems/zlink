package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;

import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory;

final class SpringEagerRuntimeInjectionTest {
    @Test
    void eagerApplicationBeanCanInjectExistingPublicRuntime() throws Exception {
        try (var context = new AnnotationConfigApplicationContext()) {
            context.registerBean(
                    ZLinkBackendAdapterProvider.class, FakeZLinkBackendAdapterFactory::new);
            context.register(
                    ZLinkFrameworkAutoConfigurationTest.TestConfig.class,
                    ZLinkFrameworkAutoConfiguration.class);
            context.registerBean(EagerConsumer.class);
            assertDoesNotThrow(context::refresh);
            assertTrue(context.getBean(EagerConsumer.class).runtime().isReady());
            assertSame(
                    context.getBean(ZLinkFrameworkRuntime.class),
                    context.getBean(EagerConsumer.class).runtime());
            context.getBean(EagerConsumer.class)
                    .serving
                    .get(3, java.util.concurrent.TimeUnit.SECONDS);
        }
    }

    static final class EagerConsumer {
        private final ZLinkFrameworkRuntime runtime;
        private final java.util.concurrent.CompletableFuture<Void> serving =
                new java.util.concurrent.CompletableFuture<>();

        EagerConsumer(ZLinkFrameworkRuntime runtime) {
            this.runtime = runtime;
            assertEquals(
                    systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState.PREPARING,
                    runtime.status().state());
            assertFalse(runtime.isReady());
            runtime.observe()
                    .subscribe(
                            new java.util.concurrent.Flow.Subscriber<
                                    systems.zlink.framework.monitoring.ZLinkObservedStatus<
                                            systems.zlink.framework.monitoring
                                                    .ZLinkFrameworkRuntimeStatus>>() {
                                public void onSubscribe(
                                        java.util.concurrent.Flow.Subscription value) {
                                    value.request(Long.MAX_VALUE);
                                }

                                public void onNext(
                                        systems.zlink.framework.monitoring.ZLinkObservedStatus<
                                                        systems.zlink.framework.monitoring
                                                                .ZLinkFrameworkRuntimeStatus>
                                                value) {
                                    if (value.status().state()
                                            == systems.zlink.framework.runtime.host
                                                    .ZLinkFrameworkRuntimeState.SERVING)
                                        serving.complete(null);
                                }

                                public void onError(Throwable error) {
                                    serving.completeExceptionally(error);
                                }

                                public void onComplete() {}
                            });
        }

        ZLinkFrameworkRuntime runtime() {
            return runtime;
        }
    }
}
