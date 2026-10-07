package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.NoUniqueBeanDefinitionException;
import org.springframework.beans.factory.annotation.Qualifier;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.lang.reflect.Proxy;

final class SpringNamedDependencyTest {
    @Test
    void activationKeepsPrototypeDependenciesSelectedByDifferentParameterNamesDistinct() {
        try (var context = new AnnotationConfigApplicationContext(PrototypeDependencies.class);
                var owner =
                        new ZLinkHandlerInstanceOwner(
                                new ZLinkSpringHandlerFactory(context.getBeanFactory()))) {
            var handler = (NamedHandler) owner.instance(NamedHandler.class);
            assertEquals("first", handler.first.value());
            assertEquals("second", handler.second.value());
            assertNotSame(handler.first, handler.second);
            var other = (SecondNamedHandler) owner.instance(SecondNamedHandler.class);
            assertSame(handler.second, other.dependency);
        }
    }

    @Test
    void spotConstructorUsesQualifiersForSameTypeBeans() {
        try (var context = context(QualifiedSpot.class)) {
            context.refresh();
            var spot = context.getBean(QualifiedSpot.class);
            assertSame(context.getBean("first"), spot.first);
            assertSame(context.getBean("second"), spot.second);
        }
    }

    @Test
    void spotConstructorUsesSpringParameterNameResolution() {
        assertTrue(NamedSpot.class.getConstructors()[0].getParameters()[1].isNamePresent());
        try (var context = context(NamedSpot.class)) {
            context.refresh();
            assertSame(context.getBean("second"), context.getBean(NamedSpot.class).dependency);
        }
    }

    @Test
    void ambiguousSpotDependencyFailsContextRefresh() {
        try (var context = context(AmbiguousSpot.class)) {
            var failure = assertThrows(RuntimeException.class, context::refresh);
            Throwable cause = failure;
            while (cause.getCause() != null) {
                cause = cause.getCause();
            }
            assertInstanceOf(NoUniqueBeanDefinitionException.class, cause);
        }
    }

    private static <T> AnnotationConfigApplicationContext context(Class<T> spotType) {
        var context = new AnnotationConfigApplicationContext();
        context.register(Dependencies.class);
        var spotContext =
                (ZLinkSpotContext)
                        Proxy.newProxyInstance(
                                ZLinkSpotContext.class.getClassLoader(),
                                new Class<?>[] {ZLinkSpotContext.class},
                                (proxy, method, arguments) -> {
                                    throw new UnsupportedOperationException(method.getName());
                                });
        context.registerBean(
                spotType,
                () ->
                        spotType.cast(
                                ZLinkHandlerActivator.services(
                                                new ZLinkSpringHandlerFactory(
                                                        context.getBeanFactory()))
                                        .add(ZLinkSpotContext.class, spotContext)
                                        .create(spotType)));
        return context;
    }

    @Configuration
    static class Dependencies {
        @Bean
        Dependency first() {
            return new Dependency();
        }

        @Bean
        Dependency second() {
            return new Dependency();
        }
    }

    static final class Dependency {}

    record NamedDependency(String value) {}

    @Configuration
    static class PrototypeDependencies {
        @Bean
        @org.springframework.context.annotation.Scope("prototype")
        NamedDependency first() {
            return new NamedDependency("first");
        }

        @Bean
        @org.springframework.context.annotation.Scope("prototype")
        NamedDependency second() {
            return new NamedDependency("second");
        }
    }

    public static final class NamedHandler {
        final NamedDependency first;
        final NamedDependency second;

        public NamedHandler(NamedDependency first, NamedDependency second) {
            this.first = first;
            this.second = second;
        }
    }

    public static final class SecondNamedHandler {
        final NamedDependency dependency;

        public SecondNamedHandler(NamedDependency second) {
            this.dependency = second;
        }
    }

    public abstract static class TestSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public java.util.concurrent.CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }

        @Override
        public java.util.concurrent.CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
    }

    public static final class QualifiedSpot extends TestSpot {
        private final ZLinkSpotContext context;
        final Dependency first;
        final Dependency second;

        public QualifiedSpot(
                ZLinkSpotContext context,
                @Qualifier("first") Dependency first,
                @Qualifier("second") Dependency second) {
            this.context = context;
            this.first = first;
            this.second = second;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }
    }

    public static final class NamedSpot extends TestSpot {
        private final ZLinkSpotContext context;
        final Dependency dependency;

        public NamedSpot(ZLinkSpotContext context, Dependency second) {
            this.context = context;
            this.dependency = second;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }
    }

    public static final class AmbiguousSpot extends TestSpot {
        private final ZLinkSpotContext context;

        public AmbiguousSpot(ZLinkSpotContext context, Dependency dependency) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }
    }
}
