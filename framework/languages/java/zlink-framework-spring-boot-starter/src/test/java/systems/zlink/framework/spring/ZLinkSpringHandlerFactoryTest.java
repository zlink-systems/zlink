package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Qualifier;
import org.springframework.beans.factory.config.BeanPostProcessor;
import org.springframework.beans.factory.config.ConfigurableBeanFactory;
import org.springframework.beans.factory.config.DependencyDescriptor;
import org.springframework.beans.factory.support.DefaultListableBeanFactory;
import org.springframework.context.ApplicationContextException;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.context.annotation.Scope;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkHandlerFilterContext;
import systems.zlink.framework.ZLinkHandlerFilterNext;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.handlers.ZLinkStreamPacket;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkSpringHandlerFactoryTest {
    @Test
    void explicitActorFactoryResultRemainsASpringBeanWithoutConstructorValidation() {
        var options = new DefaultZLinkFrameworkOptions();
        var node = options.addRouteMesh("factory.mesh");
        node.objects().server().addActorFactory("factory.actor",
                ZLinkFrameworkAutoConfigurationTest.InjectedPlayerActor.class,
                ZLinkFrameworkAutoConfigurationTest.InjectedPlayerActorFactory.class,
                factory -> factory.disableRelocation());
        var resultType = ZLinkFrameworkAutoConfigurationTest.InjectedPlayerActor.class;
        assertTrue(options.registration().applicationTypes().contains(resultType));
        Assertions.assertFalse(options.registration().constructionTypes().contains(resultType));
        assertTrue(options.registration().constructionTypes().contains(
                ZLinkFrameworkAutoConfigurationTest.InjectedPlayerActorFactory.class));
        var beans = new DefaultListableBeanFactory();
        ZLinkApplicationBeanRegistrar.register(beans, beans, options);
        assertEquals(1, beans.getBeanNamesForType(resultType, true, false).length);
    }

    @Test
    void preparedSpringConstructionUsesTheSpringActivation() {
        var factory = new ZLinkSpringHandlerFactory(new DefaultListableBeanFactory());
        var services = ZLinkHandlerActivator.services(factory);
        var plan = services.prepareConstruction(SpringCatalogHandler.class, Set.of(FieldDependency.class));
        var dependency = new FieldDependency("runtime");
        try (var activation = services.openActivation()) {
            var handler = (SpringCatalogHandler) plan.create(activation, type -> dependency);
            assertSame(dependency, handler.dependency);
        }
    }

    @Test
    void packetHandlerReceivesRuntimeSessionContextAndProcessesPacket() throws Exception {
        var backend = new FakeZLinkBackendAdapterFactory();
        ContextPacketHandler.handled = new CompletableFuture<>();
        try (var context = new AnnotationConfigApplicationContext()) {
            context.registerBean(ZLinkBackendAdapterProvider.class,
                    () -> backend);
            context.registerBean(ZLinkFrameworkConfigurer.class, () -> options -> {
                var stream = options.addStreamNode("context.stream");
                stream.bind("inproc://context-stream");
                stream.registerSession(ContextSession.class);
                stream.addSessionPacketHandler(ContextPacketHandler.class);
            });
            context.register(ChannelConfig.class, ZLinkFrameworkAutoConfiguration.class);
            context.refresh();
            backend.dispatchStreamPacket("context.packet", Message.from(
                    "{\"value\":\"payload\"}".getBytes(StandardCharsets.UTF_8)),
                    ZLinkStreamCodec.JSON);
            assertEquals("payload", ContextPacketHandler.handled.get(2, TimeUnit.SECONDS));
        }
    }

    public static final class ContextPacketHandler {
        static CompletableFuture<String> handled;
        private final ZLinkSessionContext context;
        public ContextPacketHandler(ZLinkSessionContext context) {
            this.context = context;
        }
        public Class<ContextPacket> messageType() { return ContextPacket.class; }
        public String packetName() { return "context.packet"; }
        @ZLinkStreamPacket
        public CompletionStage<Void> handle(ZLinkSessionContext context,
                ZLinkSessionDispatchContext dispatch, ContextPacket packet) {
            assertSame(this.context, context);
            handled.complete(packet.value());
            return CompletableFuture.completedFuture(null);
        }
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static final class ContextSession implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final ZLinkSessionPacketDispatcher dispatcher;
        public ContextSession(ZLinkSessionContext context,
                ZLinkSessionPacketDispatcher dispatcher) {
            this.context = context;
            this.dispatcher = dispatcher;
        }
        public ZLinkSessionContext context() { return context; }
        public CompletionStage<Void> onConnected() { return CompletableFuture.completedFuture(null); }
        public CompletionStage<Void> onDisconnected() { return CompletableFuture.completedFuture(null); }
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }
        public CompletionStage<Void> onDispatch(ZLinkSessionDispatchContext dispatch,
                ZLinkMessage payload) {
            return dispatcher.tryHandle(context, dispatch, payload).thenApply(ignored -> null);
        }
    }

    @ZLinkPacket("context.packet")
    public record ContextPacket(String value) {}

    @Test
    void spotReceivesRuntimeContextAndActivatesInSpring() throws Exception {
        ContextSpot.activated = new CompletableFuture<>();
        try (var context = new AnnotationConfigApplicationContext()) {
            context.registerBean(ZLinkBackendAdapterProvider.class,
                    ZLinkJavaBackendAdapterFactory::new);
            context.registerBean(ZLinkFrameworkConfigurer.class, () -> options -> {
                options.addLocationStore(new ZLinkInMemoryLocationStore());
                var mesh = options.addRouteMesh("context.mesh");
                mesh.listen("inproc://context-spot");
                mesh.channelName("context.mesh").server();
                mesh.objects().server().addSpotFactory("context.spot", ContextSpot.class,
                        factory -> factory.disableRelocation());
            });
            context.register(ChannelConfig.class, ZLinkFrameworkAutoConfiguration.class);
            context.refresh();
            context.getBean(ZLinkSpotManager.class)
                    .create("context.spot").inMesh("context.mesh").submit().toCompletableFuture().join();
            Assertions.assertNotNull(
                    ContextSpot.activated.get(2, TimeUnit.SECONDS));
        }
    }

    public static final class ContextSpot implements
            ZLinkSpot<ZLinkActor> {
        static CompletableFuture<ZLinkSpotContext> activated;
        private final ZLinkSpotContext context;
        public ContextSpot(ZLinkSpotContext context) { this.context = context; }
        public ZLinkSpotContext context() { return context; }
        public CompletionStage<Void> onInitialize() {
            activated.complete(context);
            return CompletableFuture.completedFuture(null);
        }
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    @Test
    void aSharedSpringFactoryKeepsRegistrationCatalogPlansSeparate() {
        var factory = new ZLinkSpringHandlerFactory(new DefaultListableBeanFactory());
        var first = ZLinkHandlerActivator.services(factory);
        var second = ZLinkHandlerActivator.services(factory);
        var dependency = new FieldDependency("runtime");
        first.add(FieldDependency.class, dependency);
        first.prepare(SpringCatalogHandler.class);
        second.prepare(SpringCatalogHandler.class);
        try (var activation = first.openActivation()) {
            assertSame(dependency, ((SpringCatalogHandler) activation.create(SpringCatalogHandler.class)).dependency);
        }
        try (var activation = second.openActivation()) {
            Assertions.assertNull(
                    ((SpringCatalogHandler) activation.create(SpringCatalogHandler.class)).dependency);
        }
    }

    public static final class SpringCatalogHandler {
        final FieldDependency dependency;
        public SpringCatalogHandler(FieldDependency dependency) { this.dependency = dependency; }
        public SpringCatalogHandler() { dependency = null; }
    }

    @Test
    void frameworkServicesAreAvailableWhenCompilingTheStartupPlan() {
        try (var context = channelContext(new CountingBeanFactory(), RuntimeHandler.class, null)) {
            assertEquals("reply", reply(context));
        }
    }

    public static final class RuntimeHandler
            implements ZLinkRequestHandler<String, String> {
        public RuntimeHandler(ZLinkFrameworkRuntime runtime) {
            Assertions.assertNotNull(runtime);
        }
        public CompletionStage<String> handle(
                String request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture("reply");
        }
    }

    @Test
    void preparedChannelConstructionNeverQueriesRejectedDependencies() {
        var beans = new CountingBeanFactory();
        try (var context = channelContext(beans, PlannedHandler.class, PlannedFilter.class)) {
            PlannedHandler.calls.set(0);
            PlannedFilter.calls.set(0);
            beans.missingSearches.set(0);
            ScopedDependency previous = null;
            for (int index = 0; index < 3; index++) {
                assertEquals("reply", reply(context));
                assertSame(PlannedHandler.dependency, PlannedFilter.dependency);
                if (previous != null) {
                    assertNotSame(previous, PlannedHandler.dependency);
                }
                previous = PlannedHandler.dependency;
            }
            assertEquals(3, PlannedHandler.calls.get());
            assertEquals(3, PlannedFilter.calls.get());
            assertEquals(0, beans.missingSearches.get());
        }
    }

    @Test
    void constructorBodyFailureDoesNotExecuteAnAlternativeConstructor() {
        ThrowingHandler.alternatives.set(0);
        try (var context = channelContext(new CountingBeanFactory(), ThrowingHandler.class, null)) {
            Assertions.assertThrows(
                    CompletionException.class, () -> reply(context));
            assertEquals(0, ThrowingHandler.alternatives.get());
        }
    }

    @Test
    void filterConstructorFailureDoesNotExecuteAnAlternativeConstructor() {
        ThrowingFilter.alternatives.set(0);
        try (var context = channelContext(new CountingBeanFactory(), PlannedHandler.class, ThrowingFilter.class)) {
            Assertions.assertThrows(
                    CompletionException.class, () -> reply(context));
            assertEquals(0, ThrowingFilter.alternatives.get());
        }
    }

    @Test
    void ambiguousConstructorFailsHostStartupWithoutCreatingPrototypeDependencies() {
        ScopedDependency.instances.set(0);
        Assertions.assertThrows(
                ApplicationContextException.class,
                () -> {
                    try (var ignored = channelContext(
                            new CountingBeanFactory(), AmbiguousHandler.class, null)) {}
                });
        assertEquals(0, ScopedDependency.instances.get());
    }

    public static final class ThrowingFilter implements ZLinkHandlerFilter {
        static final AtomicInteger alternatives = new AtomicInteger();
        public ThrowingFilter(@Qualifier("selected") ScopedDependency dependency) {
            throw new IllegalStateException("filter constructor body failed");
        }
        public ThrowingFilter() { alternatives.incrementAndGet(); }
        public <T> CompletionStage<T> invoke(
                ZLinkHandlerFilterContext context,
                ZLinkHandlerFilterNext<T> next) {
            return next.invoke();
        }
    }

    public static final class AmbiguousHandler
            implements ZLinkRequestHandler<String, String> {
        public AmbiguousHandler(ScopedDependency dependency) {
            throw new AssertionError("Ambiguous handler constructor executed");
        }
        public CompletionStage<String> handle(
                String request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture("reply");
        }
    }

    @Test
    void unavailableConstructorFailsHostStartupWithoutConstructingAnything() {
        UnavailableHandler.calls.set(0);
        ScopedDependency.instances.set(0);
        Assertions.assertThrows(
                ApplicationContextException.class,
                () -> {
                    try (var ignored = channelContext(
                            new CountingBeanFactory(), UnavailableHandler.class, null)) {}
                });
        assertEquals(0, UnavailableHandler.calls.get());
        assertEquals(0, ScopedDependency.instances.get());
    }

    private static AnnotationConfigApplicationContext channelContext(
            CountingBeanFactory beans,
            Class<? extends ZLinkRequestHandler<String, String>> handler,
            Class<?> filter) {
        var context = new AnnotationConfigApplicationContext(beans);
        context.register(ShortcutConfig.class, ChannelConfig.class);
        context.registerBean(ZLinkFrameworkConfigurer.class, () -> options -> {
            if (filter != null) {
                options.useFilter(filter.asSubclass(
                        ZLinkHandlerFilter.class));
            }
            var channel = options.addClientServerChannel("construction");
            channel.client();
            channel.server().listen().addRequestHandler(handler, String.class, String.class);
        });
        context.register(ZLinkFrameworkAutoConfiguration.class);
        try {
            context.refresh();
            return context;
        } catch (RuntimeException failure) {
            context.close();
            throw failure;
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class ChannelConfig {}

    private static String reply(AnnotationConfigApplicationContext context) {
        return context.getBean(ZLinkClient.class)
                .requestToChannel("construction", "request").submit(String.class)
                .toCompletableFuture().join();
    }

    public static final class PlannedHandler
            implements ZLinkRequestHandler<String, String> {
        static final AtomicInteger calls = new AtomicInteger();
        static ScopedDependency dependency;
        public PlannedHandler(MissingDependency missing,
                @Qualifier("selected") ScopedDependency value) {
            throw new AssertionError("Rejected constructor executed");
        }
        public PlannedHandler(@Qualifier("selected") ScopedDependency value) {
            calls.incrementAndGet();
            dependency = value;
        }
        public CompletionStage<String> handle(
                String request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture("reply");
        }
    }

    public static final class PlannedFilter implements ZLinkHandlerFilter {
        static final AtomicInteger calls = new AtomicInteger();
        static ScopedDependency dependency;
        public PlannedFilter(MissingDependency missing,
                @Qualifier("selected") ScopedDependency value) {
            throw new AssertionError("Rejected constructor executed");
        }
        public PlannedFilter(@Qualifier("selected") ScopedDependency value) {
            calls.incrementAndGet();
            dependency = value;
        }
        public <T> CompletionStage<T> invoke(
                ZLinkHandlerFilterContext context,
                ZLinkHandlerFilterNext<T> next) {
            return next.invoke();
        }
    }

    public static final class ThrowingHandler
            implements ZLinkRequestHandler<String, String> {
        static final AtomicInteger alternatives = new AtomicInteger();
        public ThrowingHandler(@Qualifier("selected") ScopedDependency dependency) {
            throw new IllegalStateException("constructor body failed");
        }
        public ThrowingHandler() { alternatives.incrementAndGet(); }
        public CompletionStage<String> handle(
                String request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture("reply");
        }
    }

    public static final class UnavailableHandler
            implements ZLinkRequestHandler<String, String> {
        static final AtomicInteger calls = new AtomicInteger();
        public UnavailableHandler(
                @Qualifier("selected") ScopedDependency dependency, MissingDependency missing) {
            calls.incrementAndGet();
        }
        public CompletionStage<String> handle(
                String request, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture("reply");
        }
    }

    @Test
    void optionalServiceLookupPreservesCreationFailureAndReturnsNullOnlyForAbsence() {
        var beans = new DefaultListableBeanFactory();
        var factory = new ZLinkSpringHandlerFactory(beans);
        org.junit.jupiter.api.Assertions.assertNull(factory.findService(FieldDependency.class));
        var original = new IllegalStateException("service creation failed");
        var definition =
                new org.springframework.beans.factory.support.RootBeanDefinition(
                        FieldDependency.class);
        definition.setInstanceSupplier(
                () -> {
                    throw original;
                });
        beans.registerBeanDefinition("dependency", definition);
        var failure =
                org.junit.jupiter.api.Assertions.assertThrows(
                        org.springframework.beans.factory.BeanCreationException.class,
                        () -> factory.findService(FieldDependency.class));
        assertSame(original, failure.getMostSpecificCause());
    }

    @Test
    void ignoresUnrelatedAutowiredAnnotationWithTheSameSimpleName() throws Exception {
        try (AnnotationConfigApplicationContext context =
                new AnnotationConfigApplicationContext(ShortcutConfig.class)) {
            var factory = new ZLinkSpringHandlerFactory(context.getBeanFactory());
            try (var activation = factory.openActivation()) {
                var handler =
                        (AnnotationIdentityHandler)
                                activation.create(AnnotationIdentityHandler.class);
                assertTrue(handler.springConstructorSelected());
            }
        }
    }

    @Test
    void unrelatedPreDestroyAnnotationDoesNotSuppressAutoCloseable() {
        var factory = new ZLinkSpringHandlerFactory(new DefaultListableBeanFactory());
        var handler =
                (AnnotationIdentityCloseable) factory.create(AnnotationIdentityCloseable.class);
        factory.destroy(handler);
        assertEquals(1, handler.closeCount());
    }

    private static final class UnrelatedAnnotations {
        @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
        @java.lang.annotation.Target(java.lang.annotation.ElementType.CONSTRUCTOR)
        @interface Autowired {}

        @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
        @java.lang.annotation.Target(java.lang.annotation.ElementType.METHOD)
        @interface PreDestroy {}
    }

    public static final class AnnotationIdentityHandler {
        private final boolean springConstructorSelected;

        @Autowired
        public AnnotationIdentityHandler(FieldDependency dependency) {
            springConstructorSelected = true;
        }

        @UnrelatedAnnotations.Autowired
        public AnnotationIdentityHandler(
                FieldDependency dependency, @Qualifier("selected") ScopedDependency scoped) {
            springConstructorSelected = false;
        }

        public boolean springConstructorSelected() {
            return springConstructorSelected;
        }
    }

    public static final class AnnotationIdentityCloseable implements AutoCloseable {
        private int closeCount;

        @Override
        @UnrelatedAnnotations.PreDestroy
        public void close() {
            closeCount++;
        }

        public int closeCount() {
            return closeCount;
        }
    }

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
            assertEquals(
                    0,
                    ScopedDependency.instances.get(),
                    "preparation must not create a scoped dependency");

            try (var owner = new ZLinkHandlerInstanceOwner(factory)) {
                PreparedHandler handler = (PreparedHandler) owner.instance(PreparedHandler.class);
                assertEquals("selected", handler.dependency.value);
            }

            assertEquals(
                    0,
                    beanFactory.candidateSearches(),
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
                ShortcutHandler handler = (ShortcutHandler) owner.instance(ShortcutHandler.class);
                ShortcutFilter filter = (ShortcutFilter) owner.instance(ShortcutFilter.class);
                first = handler.dependency;

                assertEquals("selected", first.value);
                assertSame(first, filter.dependency);
                assertEquals("field", handler.field.value);
                assertEquals("method", handler.method.value);
            }
            int candidateSearchesAfterFirstActivation = beanFactory.candidateSearches();
            assertTrue(candidateSearchesAfterFirstActivation > 0);
            assertEquals(2, ShortcutPostProcessor.initialized.get());

            try (var owner = new ZLinkHandlerInstanceOwner(factory)) {
                ShortcutHandler handler = (ShortcutHandler) owner.instance(ShortcutHandler.class);
                ShortcutFilter filter = (ShortcutFilter) owner.instance(ShortcutFilter.class);

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
                AggregateHandler aggregate =
                        (AggregateHandler) owner.instance(AggregateHandler.class);
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
            int candidateSearchesAfterFirstActivation = beanFactory.candidateSearches();

            try (var owner = new ZLinkHandlerInstanceOwner(factory)) {
                AggregateHandler aggregate =
                        (AggregateHandler) owner.instance(AggregateHandler.class);
                SingleAggregateHandler singleAggregate =
                        (SingleAggregateHandler) owner.instance(SingleAggregateHandler.class);
                LazyHandler lazy = (LazyHandler) owner.instance(LazyHandler.class);

                assertNotSame(firstAggregate, aggregate.dependencies);
                assertEquals(2, aggregate.dependencies.size());
                assertNotSame(firstSingleAggregate, singleAggregate.dependencies);
                assertEquals(1, singleAggregate.dependencies.size());
                assertSame(
                        firstSingleAggregate.getFirst(), singleAggregate.dependencies.getFirst());
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
        private final AtomicInteger missingSearches = new AtomicInteger();

        @Override
        protected Map<String, Object> findAutowireCandidates(
                String beanName, Class<?> requiredType, DependencyDescriptor descriptor) {
            candidateSearches.incrementAndGet();
            if (requiredType == MissingDependency.class) {
                missingSearches.incrementAndGet();
            }
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
        @Autowired private FieldDependency field;
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

        public PreparedHandler(@Qualifier("selected") ScopedDependency dependency) {
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

    static final class MissingDependency {}

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
