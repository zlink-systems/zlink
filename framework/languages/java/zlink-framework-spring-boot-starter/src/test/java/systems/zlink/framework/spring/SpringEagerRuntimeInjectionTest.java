package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;

import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory;

final class SpringEagerRuntimeInjectionTest {
    private static final long OBSERVATION_TIMEOUT_SECONDS = 3;

    @Test
    void eagerTopologyReadinessObservesPreparingAndStartedMesh() throws Exception {
        try (var context = new AnnotationConfigApplicationContext()) {
            context.registerBean(
                    ZLinkBackendAdapterProvider.class,
                    systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory::new);
            context.registerBean(
                    systems.zlink.contracts.core.RoutingId.class,
                    () -> systems.zlink.contracts.core.RoutingId.from("eager-route"));
            context.register(
                    ZLinkFrameworkAutoConfigurationTest.RouteMeshHandlerConfig.class,
                    ZLinkFrameworkAutoConfiguration.class);
            context.registerBean(EagerTopologyConsumer.class);
            assertDoesNotThrow(context::refresh);
            var consumer = context.getBean(EagerTopologyConsumer.class);
            assertTrue(consumer.meshes.isReady("route"));
            consumer.ready.get(OBSERVATION_TIMEOUT_SECONDS, java.util.concurrent.TimeUnit.SECONDS);
        }
    }

    static final class EagerTopologyConsumer {
        final systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime meshes;
        final java.util.concurrent.CompletableFuture<Void> ready =
                new java.util.concurrent.CompletableFuture<>();

        EagerTopologyConsumer(systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime meshes) {
            this.meshes = meshes;
            assertFalse(meshes.isReady("route"));
            assertEquals(
                    systems.zlink.framework.monitoring.ZLinkTopologyState.STARTING,
                    meshes.snapshot("route").state());
            assertThrows(
                    systems.zlink.framework.errors.ZLinkConfigurationException.class,
                    () -> meshes.snapshot("missing"));
            var initial =
                    new java.util.concurrent.CompletableFuture<
                            systems.zlink.framework.monitoring.ZLinkTopologyState>();
            meshes.observe("route", 16)
                    .subscribe(
                            new java.util.concurrent.Flow.Subscriber<>() {
                                public void onSubscribe(
                                        java.util.concurrent.Flow.Subscription subscription) {
                                    subscription.request(Long.MAX_VALUE);
                                }

                                public void onNext(
                                        systems.zlink.framework.monitoring.ZLinkObservedStatus<
                                                        systems.zlink.framework.monitoring
                                                                .ZLinkMeshNodeSnapshot>
                                                value) {
                                    initial.complete(value.status().state());
                                    if (value.status().isReady()) ready.complete(null);
                                }

                                public void onError(Throwable error) {
                                    initial.completeExceptionally(error);
                                    ready.completeExceptionally(error);
                                }

                                public void onComplete() {}
                            });
            assertEquals(
                    systems.zlink.framework.monitoring.ZLinkTopologyState.STARTING,
                    assertDoesNotThrow(
                            () ->
                                    initial.get(
                                            OBSERVATION_TIMEOUT_SECONDS,
                                            java.util.concurrent.TimeUnit.SECONDS)));
        }
    }

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
                    .get(OBSERVATION_TIMEOUT_SECONDS, java.util.concurrent.TimeUnit.SECONDS);
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
