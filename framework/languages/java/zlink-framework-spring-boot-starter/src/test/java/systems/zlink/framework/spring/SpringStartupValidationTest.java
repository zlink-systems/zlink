package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology;
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory;

final class SpringStartupValidationTest {
    @Test
    void configuredAmbiguousSpotFailsRuntimeStartup() {
        try (var context = new AnnotationConfigApplicationContext()) {
            context.registerBean(
                    ZLinkBackendAdapterProvider.class, FakeZLinkBackendAdapterFactory::new);
            context.registerBean(ZLinkFrameworkEnabled.class, ZLinkFrameworkEnabled::new);
            context.register(
                    SpringNamedDependencyTest.Dependencies.class,
                    ZLinkFrameworkAutoConfiguration.class);
            context.registerBean(
                    ZLinkFrameworkConfigurer.class,
                    () ->
                            options -> {
                                var node =
                                        ZLinkLegacyTopology.addSpotMesh(
                                                options, "ambiguous-startup");
                                node.enableRouter("inproc://ambiguous-startup");
                                node.objects()
                                        .server()
                                        .addSpotFactory(
                                                "ambiguous",
                                                SpringNamedDependencyTest.AmbiguousSpot.class,
                                                factory -> factory.disableRelocation());
                            });
            assertThrows(RuntimeException.class, context::refresh);
        }
    }
}
