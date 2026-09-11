package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Qualifier;
import org.springframework.beans.factory.config.BeanPostProcessor;
import org.springframework.beans.factory.config.ConfigurableBeanFactory;
import org.springframework.beans.factory.config.DependencyDescriptor;
import org.springframework.beans.factory.support.DefaultListableBeanFactory;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.context.annotation.Scope;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;

final class ZLinkSpringHandlerFactoryTest {
    @Test
    void preparesConstructorAndScalarDependencyBeforeFirstActivation() throws Exception {
        CountingBeanFactory beanFactory = new CountingBeanFactory();
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext(beanFactory)) {
            context.register(ShortcutConfig.class);
            context.refresh();
            ZLinkSpringHandlerFactory factory = new ZLinkSpringHandlerFactory(beanFactory);
            ScopedDependency.instances.set(0);

            factory.prepare(PreparedHandler.class);
            beanFactory.resetCandidateSearches();
            assertEquals(0, ScopedDependency.instances.get(),
                "preparation must not create a scoped dependency");

            try (var owner = new ZLinkHandlerInstanceOwner(factory)) {
                PreparedHandler handler = (PreparedHandler) owner.instance(
                    PreparedHandler.class);
                assertEquals("selected", handler.dependency.value);
            }

            assertEquals(0, beanFactory.candidateSearches(),
                "the first activation must use registration-time dependency metadata");
            assertEquals(1, ScopedDependency.instances.get());
        }
    }

    @Test
    void cachesFrozenScalarCandidateWithoutChangingScopeOrBeanLifecycle() {
        CountingBeanFactory beanFactory = new CountingBeanFactory();
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext(beanFactory)) {
            context.register(ShortcutConfig.class);
            context.refresh();
            beanFactory.resetCandidateSearches();
            ShortcutPostProcessor.reset();
            ZLinkSpringHandlerFactory factory = new ZLinkSpringHandlerFactory(beanFactory);

            ScopedDependency first;
            try (var owner = new ZLinkHandlerInstanceOwner(factory)) {
                ShortcutHandler handler = (ShortcutHandler) owner.instance(
                    ShortcutHandler.class);
                ShortcutFilter filter = (ShortcutFilter) owner.instance(
                    ShortcutFilter.class);
                first = handler.dependency;

                assertEquals("selected", first.value);
                assertSame(first, filter.dependency);
                assertEquals("field", handler.field.value);
                assertEquals("method", handler.method.value);
            }
            int candidateSearchesAfterFirstActivation =
                beanFactory.candidateSearches();
            assertTrue(candidateSearchesAfterFirstActivation > 0);
            assertEquals(2, ShortcutPostProcessor.initialized.get());

            try (var owner = new ZLinkHandlerInstanceOwner(factory)) {
                ShortcutHandler handler = (ShortcutHandler) owner.instance(
                    ShortcutHandler.class);
                ShortcutFilter filter = (ShortcutFilter) owner.instance(
                    ShortcutFilter.class);

                assertNotSame(first, handler.dependency);
                assertSame(handler.dependency, filter.dependency);
                assertEquals("field", handler.field.value);
                assertEquals("method", handler.method.value);
            }

            assertEquals(
                candidateSearchesAfterFirstActivation,
                beanFactory.candidateSearches(),
                "the second activation must use the cached name shortcut");
            assertEquals(4, ShortcutPostProcessor.initialized.get());
        }
    }

    @Test
    void preservesAggregateOptionalAndProviderResolutionOutsideTheShortcut() {
        CountingBeanFactory beanFactory = new CountingBeanFactory();
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext(beanFactory)) {
            context.register(ShortcutConfig.class);
            context.refresh();
            beanFactory.resetCandidateSearches();
            ZLinkSpringHandlerFactory factory = new ZLinkSpringHandlerFactory(beanFactory);

            List<ScopedDependency> firstAggregate;
            List<FieldDependency> firstSingleAggregate;
            ScopedDependency firstProvided;
            try (var owner = new ZLinkHandlerInstanceOwner(factory)) {
                AggregateHandler aggregate = (AggregateHandler) owner.instance(
                    AggregateHandler.class);
                SingleAggregateHandler singleAggregate =
                    (SingleAggregateHandler) owner.instance(SingleAggregateHandler.class);
                LazyHandler lazy = (LazyHandler) owner.instance(LazyHandler.class);
                firstAggregate = aggregate.dependencies;
                firstSingleAggregate = singleAggregate.dependencies;
                firstProvided = lazy.selected.getObject();

                assertEquals(2, firstAggregate.size());
                assertEquals(1, firstSingleAggregate.size());
                assertEquals("field", firstSingleAggregate.getFirst().value);
                assertTrue(lazy.missing.isEmpty());
                assertEquals("selected", firstProvided.value);
            }
            int candidateSearchesAfterFirstActivation =
                beanFactory.candidateSearches();

            try (var owner = new ZLinkHandlerInstanceOwner(factory)) {
                AggregateHandler aggregate = (AggregateHandler) owner.instance(
                    AggregateHandler.class);
                SingleAggregateHandler singleAggregate =
                    (SingleAggregateHandler) owner.instance(SingleAggregateHandler.class);
                LazyHandler lazy = (LazyHandler) owner.instance(LazyHandler.class);

                assertNotSame(firstAggregate, aggregate.dependencies);
                assertEquals(2, aggregate.dependencies.size());
                assertNotSame(firstSingleAggregate, singleAggregate.dependencies);
                assertEquals(1, singleAggregate.dependencies.size());
                assertSame(firstSingleAggregate.getFirst(), singleAggregate.dependencies.getFirst());
                assertTrue(lazy.missing.isEmpty());
                assertNotSame(firstProvided, lazy.selected.getObject());
            }

            assertTrue(
                beanFactory.candidateSearches() > candidateSearchesAfterFirstActivation,
                "aggregate resolution must remain on Spring's normal path");
        }
    }

    @Configuration
    static class ShortcutConfig {
        @Bean("selected")
        @Qualifier("selected")
        @Scope(ConfigurableBeanFactory.SCOPE_PROTOTYPE)
        ScopedDependency selectedDependency() {
            return new ScopedDependency("selected");
        }

        @Bean("other")
        @Qualifier("other")
        @Scope(ConfigurableBeanFactory.SCOPE_PROTOTYPE)
        ScopedDependency otherDependency() {
            return new ScopedDependency("other");
        }

        @Bean
        FieldDependency fieldDependency() {
            return new FieldDependency("field");
        }

        @Bean
        MethodDependency methodDependency() {
            return new MethodDependency("method");
        }

        @Bean
        ShortcutPostProcessor shortcutPostProcessor() {
            return new ShortcutPostProcessor();
        }
    }

    static final class CountingBeanFactory extends DefaultListableBeanFactory {
        private final AtomicInteger candidateSearches = new AtomicInteger();

        @Override
        protected Map<String, Object> findAutowireCandidates(
            String beanName,
            Class<?> requiredType,
            DependencyDescriptor descriptor) {
            candidateSearches.incrementAndGet();
            return super.findAutowireCandidates(beanName, requiredType, descriptor);
        }

        int candidateSearches() {
            return candidateSearches.get();
        }

        void resetCandidateSearches() {
            candidateSearches.set(0);
        }
    }

    public static final class ShortcutHandler {
        private final ScopedDependency dependency;
        @Autowired
        private FieldDependency field;
        private MethodDependency method;

        public ShortcutHandler(@Qualifier("selected") ScopedDependency dependency) {
            this.dependency = dependency;
        }

        @Autowired
        void setMethod(MethodDependency value) {
            method = value;
        }
    }

    public static final class PreparedHandler {
        private final ScopedDependency dependency;

        public PreparedHandler(
            @Qualifier("selected") ScopedDependency dependency) {
            this.dependency = dependency;
        }
    }

    public static final class ShortcutFilter {
        private final ScopedDependency dependency;

        public ShortcutFilter(@Qualifier("selected") ScopedDependency dependency) {
            this.dependency = dependency;
        }
    }

    public static final class AggregateHandler {
        private final List<ScopedDependency> dependencies;

        public AggregateHandler(List<ScopedDependency> dependencies) {
            this.dependencies = dependencies;
        }
    }

    public static final class SingleAggregateHandler {
        private final List<FieldDependency> dependencies;

        public SingleAggregateHandler(List<FieldDependency> dependencies) {
            this.dependencies = dependencies;
        }
    }

    public static final class LazyHandler {
        private final Optional<MissingDependency> missing;
        private final ObjectProvider<ScopedDependency> selected;

        public LazyHandler(
            Optional<MissingDependency> missing,
            @Qualifier("selected") ObjectProvider<ScopedDependency> selected) {
            this.missing = missing;
            this.selected = selected;
        }
    }

    static final class ScopedDependency {
        private static final AtomicInteger instances = new AtomicInteger();
        private final String value;

        ScopedDependency(String value) {
            instances.incrementAndGet();
            this.value = value;
        }
    }

    static final class FieldDependency {
        private final String value;

        FieldDependency(String value) {
            this.value = value;
        }
    }

    static final class MethodDependency {
        private final String value;

        MethodDependency(String value) {
            this.value = value;
        }
    }

    static final class MissingDependency {
    }

    static final class ShortcutPostProcessor implements BeanPostProcessor {
        private static final AtomicInteger initialized = new AtomicInteger();

        static void reset() {
            initialized.set(0);
        }

        @Override
        public Object postProcessAfterInitialization(Object bean, String beanName) {
            if (bean instanceof ShortcutHandler || bean instanceof ShortcutFilter) {
                initialized.incrementAndGet();
            }
            return bean;
        }
    }
}
