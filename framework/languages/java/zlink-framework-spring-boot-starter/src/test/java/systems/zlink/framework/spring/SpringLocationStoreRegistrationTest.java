package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Configuration;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory;

class SpringLocationStoreRegistrationTest {
    @Test
    void locationStoreBeanAloneLeavesRuntimeWithoutAStore() {
        try (var context = context(0)) {
            context.refresh();
            var failure =
                    assertThrows(
                            ZLinkConfigurationException.class,
                            () ->
                                    context.getBean(ZLinkFrameworkRuntime.class)
                                            .spotHandleResolver());
            assertEquals(
                    "SpotHandleResolver requires a configured location store",
                    failure.getMessage());
        }
    }

    @Test
    void oneExplicitRegistrationWorksWithALocationStoreBean() {
        try (var context = context(1)) {
            context.refresh();
            assertNotNull(context.getBean(ZLinkFrameworkRuntime.class).spotHandleResolver());
        }
    }

    @Test
    void twoExplicitRegistrationsRejectTheSecondCall() {
        try (var context = context(2)) {
            var failure = assertThrows(RuntimeException.class, context::refresh);
            Throwable cause = failure;
            while (cause.getCause() != null) {
                cause = cause.getCause();
            }
            assertTrue(cause instanceof ZLinkConfigurationException, cause.toString());
            assertTrue(cause.getMessage().contains("already registered"), cause.getMessage());
        }
    }

    private static AnnotationConfigApplicationContext context(int registrations) {
        var context = new AnnotationConfigApplicationContext();
        context.registerBean(
                ZLinkBackendAdapterProvider.class, FakeZLinkBackendAdapterFactory::new);
        var store = new ZLinkInMemoryLocationStore();
        context.registerBean(ZLinkLocationStore.class, () -> store);
        context.registerBean(
                ZLinkFrameworkConfigurer.class,
                () ->
                        options -> {
                            for (int index = 0; index < registrations; index++) {
                                options.addLocationStore(store);
                            }
                        });
        context.register(EnabledConfig.class, ZLinkFrameworkAutoConfiguration.class);
        return context;
    }

    @Configuration
    @EnableZLinkFramework
    static class EnabledConfig {}
}
