package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.springframework.context.ApplicationContextException;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;

final class StartupFailureTest {
    @Test
    void applicationBeanInjectsPublicRuntimeProviderAndObservesServing() throws Exception {
        try (var context =
                context(
                        new ZLinkInMemoryProviderLocationStore(),
                        "inproc://public-runtime-status")) {
            context.registerBean(PublicRuntimeConsumer.class);
            context.refresh();
            var runtime = context.getBean(PublicRuntimeConsumer.class).runtime().getObject();
            assertSame(context.getBean(ZLinkFrameworkRuntime.class), runtime);
            assertEquals(
                    systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState.SERVING,
                    runtime.status().state());
            awaitReady(runtime);
        }
    }

    record PublicRuntimeConsumer(
            org.springframework.beans.factory.ObjectProvider<ZLinkFrameworkRuntime> runtime) {}

    @Test
    void routingIdConflictFailsContextRefresh() throws Exception {
        ZLinkLocationStore store = new ZLinkInMemoryProviderLocationStore();
        try (var owner = context(store, "inproc://startup-owner");
                var conflict = context(store, "inproc://startup-conflict")) {
            owner.refresh();
            awaitReady(owner.getBean(ZLinkFrameworkRuntime.class));
            assertThrows(ApplicationContextException.class, conflict::refresh);
        }
    }

    @Test
    void successfulRefreshWaitsForRuntimeStartup() {
        try (var context =
                context(new ZLinkInMemoryProviderLocationStore(), "inproc://startup-ready")) {
            context.refresh();
            assertTrue(context.getBean(ZLinkFrameworkRuntime.class).isReady());
        }
    }

    private static void awaitReady(ZLinkFrameworkRuntime runtime) throws Exception {
        var ready = new CompletableFuture<Void>();
        runtime.observe()
                .subscribe(
                        new Flow.Subscriber<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>() {
                            private Flow.Subscription subscription;

                            @Override
                            public void onSubscribe(Flow.Subscription value) {
                                subscription = value;
                                value.request(Long.MAX_VALUE);
                            }

                            @Override
                            public void onNext(
                                    ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus> value) {
                                if (value.status().state()
                                        == systems.zlink.framework.runtime.host
                                                .ZLinkFrameworkRuntimeState.SERVING) {
                                    ready.complete(null);
                                    subscription.cancel();
                                }
                            }

                            @Override
                            public void onError(Throwable failure) {
                                ready.completeExceptionally(failure);
                            }

                            @Override
                            public void onComplete() {
                                ready.completeExceptionally(
                                        new IllegalStateException("runtime stopped before ready"));
                            }
                        });
        ready.get(3, TimeUnit.SECONDS);
    }

    private static AnnotationConfigApplicationContext context(
            ZLinkLocationStore store, String endpoint) {
        var context = new AnnotationConfigApplicationContext();
        context.registerBean(ZLinkFrameworkEnabled.class, ZLinkFrameworkEnabled::new);
        context.registerBean(ZLinkLocationStore.class, () -> store);
        context.registerBean(
                ZLinkBackendAdapterProvider.class, FakeZLinkBackendAdapterFactory::new);
        context.registerBean(
                ZLinkFrameworkConfigurer.class,
                () ->
                        options ->
                                options.addRouteMesh("startup")
                                        .listen(endpoint)
                                        .setRoutingId(RoutingId.from("startup-conflict")));
        context.register(ZLinkFrameworkAutoConfiguration.class);
        return context;
    }
}
