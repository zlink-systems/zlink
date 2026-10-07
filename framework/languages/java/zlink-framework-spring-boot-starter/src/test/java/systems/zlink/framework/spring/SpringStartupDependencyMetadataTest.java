package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.NoSuchBeanDefinitionException;
import org.springframework.beans.factory.NoUniqueBeanDefinitionException;
import org.springframework.beans.factory.annotation.Qualifier;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;

import systems.zlink.framework.spots.ZLinkSpotContext;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicInteger;

final class SpringStartupDependencyMetadataTest {
    @Test
    void missingRequiredDependencyFailsPreparation() {
        try (var context = new AnnotationConfigApplicationContext()) {
            context.refresh();
            assertThrows(
                    NoSuchBeanDefinitionException.class,
                    () ->
                            new ZLinkSpringHandlerFactory(context.getBeanFactory())
                                    .prepare(RequiredSpot.class));
        }
    }

    @Test
    void missingOptionalAndCollectionDependenciesAreAvailable() {
        try (var context = new AnnotationConfigApplicationContext()) {
            context.refresh();
            assertDoesNotThrow(
                    () ->
                            new ZLinkSpringHandlerFactory(context.getBeanFactory())
                                    .prepare(OptionalSpot.class));
        }
    }

    @Test
    void qualifiersAndParameterNamesSelectWithoutCreatingPrototypeDependenciesOrSpots() {
        var created = new AtomicInteger();
        try (var context = new AnnotationConfigApplicationContext()) {
            for (String name : List.of("first", "second")) {
                context.registerBean(
                        name,
                        Dependency.class,
                        () -> {
                            created.incrementAndGet();
                            return new Dependency();
                        },
                        definition -> definition.setScope("prototype"));
            }
            context.refresh();
            var factory = new ZLinkSpringHandlerFactory(context.getBeanFactory());
            assertThrows(
                    NoUniqueBeanDefinitionException.class,
                    () -> factory.prepare(RequiredSpot.class));
            assertDoesNotThrow(() -> factory.prepare(QualifiedSpot.class));
            assertDoesNotThrow(() -> factory.prepare(NamedSpot.class));
            assertEquals(0, created.get());
        }
    }

    static final class Dependency {}

    public abstract static class MetadataSpot extends SpringNamedDependencyTest.TestSpot {
        @Override
        public ZLinkSpotContext context() {
            throw new AssertionError("Startup validation must not invoke a Spot");
        }
    }

    public static final class RequiredSpot extends MetadataSpot {
        public RequiredSpot(ZLinkSpotContext context, Dependency dependency) {

            throw new AssertionError("Startup validation must not construct a Spot");
        }
    }

    public static final class OptionalSpot extends MetadataSpot {
        public OptionalSpot(
                ZLinkSpotContext context,
                Optional<Dependency> dependency,
                List<Dependency> dependencies) {

            throw new AssertionError("Startup validation must not construct a Spot");
        }
    }

    public static final class QualifiedSpot extends MetadataSpot {
        public QualifiedSpot(ZLinkSpotContext context, @Qualifier("second") Dependency dependency) {

            throw new AssertionError("Startup validation must not construct a Spot");
        }
    }

    public static final class NamedSpot extends MetadataSpot {
        public NamedSpot(ZLinkSpotContext context, Dependency second) {

            throw new AssertionError("Startup validation must not construct a Spot");
        }
    }
}
