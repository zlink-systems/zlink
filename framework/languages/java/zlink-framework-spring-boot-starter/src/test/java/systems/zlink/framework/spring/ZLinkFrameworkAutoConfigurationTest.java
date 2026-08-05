package systems.zlink.framework.spring;

import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.net.ServerSocket;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.DisposableBean;
import org.springframework.beans.factory.config.ConfigurableBeanFactory;
import org.springframework.beans.factory.NoSuchBeanDefinitionException;
import org.springframework.context.annotation.AnnotationConfigApplicationContext;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.context.annotation.Scope;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkHandlerFilterContext;
import systems.zlink.framework.ZLinkHandlerFilterNext;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.handlers.ZLinkRequest;
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery;
import systems.zlink.framework.locations.ZLinkLocationRuntimeStatus;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateCall;
import systems.zlink.framework.spots.ZLinkSpotGetOrCreateCall;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotOutbound;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;
import systems.zlink.framework.testkit.FakeZLinkBackendAdapterFactory;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.spring.sessionfixtures.SubpackageDiscoveredPacketSession;
import systems.zlink.framework.spring.sessionfixtures.SubpackageSessionPacketAnchor;

final class ZLinkFrameworkAutoConfigurationTest {
    private static final AtomicInteger NEXT_PORT =
        new AtomicInteger(31_000 + (int) (ProcessHandle.current().pid() % 1_000));

    @Test
    void autoConfigurationStartsFrameworkLifecycleAndExposesClientBean() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(TestConfig.class, ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            ZLinkFrameworkLifecycle lifecycle =
                context.getBean(ZLinkFrameworkLifecycle.class);
            ZLinkClient client = context.getBean(ZLinkClient.class);
            ZLinkChannelRuntimeOptions runtimeOptions =
                context.getBean(ZLinkChannelRuntimeOptions.class);
            ZLinkFanoutClient fanout = context.getBean(ZLinkFanoutClient.class);
            ZLinkRouteClient route = context.getBean(ZLinkRouteClient.class);
            ZLinkRouteMeshRuntime routeMeshRuntime =
                context.getBean(ZLinkRouteMeshRuntime.class);
            ZLinkRouteMeshRuntimeOptions routeMeshRuntimeOptions =
                context.getBean(ZLinkRouteMeshRuntimeOptions.class);
            ZLinkFrameworkRuntime runtime =
                context.getBean(ZLinkFrameworkRuntime.class);

            assertTrue(lifecycle.isRunning());
            assertSame(runtime, context.getBean(ZLinkFrameworkRuntime.class));
            assertSame(runtime, lifecycle.runtimeBean());
            assertInstanceOf(ZLinkFrameworkLifecycle.class, client);
            assertInstanceOf(ZLinkFrameworkLifecycle.class, runtimeOptions);
            assertInstanceOf(ZLinkFrameworkLifecycle.class, fanout);
            assertInstanceOf(ZLinkFrameworkLifecycle.class, route);
            assertSame(lifecycle.routeMeshRuntime(), routeMeshRuntime);
            assertSame(
                lifecycle.clientServerRuntime(),
                context.getBean(
                    systems.zlink.framework.monitoring
                        .ZLinkClientServerRuntime.class));
            assertSame(
                lifecycle.fanoutRuntime(),
                context.getBean(
                    systems.zlink.framework.monitoring.ZLinkFanoutRuntime.class));
            assertThrows(
                ZLinkConfigurationException.class,
                () -> routeMeshRuntime.snapshot("missing"));
            assertInstanceOf(
                systems.zlink.framework.spring.internal.runtime.ZLinkRouteMeshRuntimeOptionsService.class,
                routeMeshRuntimeOptions);
        }
    }

    @Test
    void enableZLinkFrameworkImportsFrameworkAutoConfiguration() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(EnabledTestConfig.class);
            context.refresh();

            assertTrue(context.getBean(ZLinkFrameworkLifecycle.class).isRunning());
            assertInstanceOf(
                ZLinkFrameworkLifecycle.class,
                context.getBean(ZLinkClient.class));
        }
    }

    @Test
    void frameworkConfigurerPassesStreamCompressionToRuntimeRegistration() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(
                StreamCompressionConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            DefaultZLinkFrameworkOptions options =
                context.getBean(DefaultZLinkFrameworkOptions.class);
            assertSame(
                StreamCompressionConfig.COMPRESSION,
                options.registration().streamCompressionCodec());
        }
    }

    @Test
    void multiTargetClientsThrowConfigurationExceptionWhenChannelIsMissing() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(TestConfig.class, ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            ZLinkFanoutClient fanout = context.getBean(ZLinkFanoutClient.class);
            ZLinkRouteClient route = context.getBean(ZLinkRouteClient.class);

            assertThrows(ZLinkConfigurationException.class, () ->
                fanout.publish("missing", "payload").submit());
            assertThrows(ZLinkConfigurationException.class, () ->
                route.requestToNode("missing", RoutingId.from("target"), "payload"));
        }
    }

    @Test
    void spotAndActorManagersAreNotBeansWithoutSpotNode() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(TestConfig.class, ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            assertThrows(NoSuchBeanDefinitionException.class, () ->
                context.getBean(ZLinkSpotManager.class));
            assertThrows(NoSuchBeanDefinitionException.class, () ->
                context.getBean(ZLinkSpotOutbound.class));
            assertThrows(NoSuchBeanDefinitionException.class, () ->
                context.getBean(ZLinkSpotPublisherClient.class));
            assertThrows(NoSuchBeanDefinitionException.class, () ->
                context.getBean(ZLinkActorManager.class));
            assertThrows(NoSuchBeanDefinitionException.class, () ->
                context.getBean(ZLinkActorDirectory.class));
        }
    }

    @Test
    void spotManagerIsBeanWhenSpotNodeExists() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(
                SpotNodeConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            ZLinkSpotManager manager =
                context.getBean(ZLinkSpotManager.class);
            assertInstanceOf(ZLinkSpotManager.class, manager);
            assertInstanceOf(
                ZLinkSpotCreateCall.class,
                manager.create("room-v1")
                    .inMesh("game")
                    .timeout(Duration.ofSeconds(1)));
            assertInstanceOf(
                ZLinkSpotGetOrCreateCall.class,
                manager.getOrCreate(
                        "spring-room",
                        "room-v1")
                    .inMesh("game")
                    .request(ZLinkMessage.empty()));
            assertInstanceOf(
                ZLinkSpotOutbound.class,
                context.getBean(ZLinkSpotOutbound.class));
            assertThrows(NoSuchBeanDefinitionException.class, () ->
                context.getBean(ZLinkSpotPublisherClient.class));
            assertThrows(NoSuchBeanDefinitionException.class, () ->
                context.getBean(ZLinkActorManager.class));
            assertThrows(NoSuchBeanDefinitionException.class, () ->
                context.getBean(ZLinkActorDirectory.class));
            ZLinkSpotOutbound outbound = context.getBean(ZLinkSpotOutbound.class);
            assertThrows(ZLinkConfigurationException.class, () ->
                outbound.sendToChannel("events", "hello"));
        }
    }

    @Test
    void spotManagerIsBeanWhenObjectClientExists() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(
                ObjectClientConfig.class,
                LocationStoreBeanConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            assertInstanceOf(
                ZLinkSpotManager.class,
                context.getBean(ZLinkSpotManager.class));
            assertInstanceOf(
                ZLinkSpotOutbound.class,
                context.getBean(ZLinkSpotOutbound.class));
            assertInstanceOf(
                ZLinkActorManager.class,
                context.getBean(ZLinkActorManager.class));
        }
    }

    @Test
    void actorClientAndDirectoryAreBeansWhenSpotNodeAndLocationStoreExist() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(
                SpotNodeWithLocationStoreConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            assertInstanceOf(
                ZLinkFrameworkActorClientBean.class,
                context.getBean(ZLinkActorClient.class));
            assertInstanceOf(
                ZLinkFrameworkActorDirectoryBean.class,
                context.getBean(ZLinkActorDirectory.class));
            assertTrue(context.getBean(ZLinkActorDirectory.class)
                .find("missing-actor")
                .toCompletableFuture()
                .join()
                .isEmpty());
            assertThrows(NoSuchBeanDefinitionException.class, () ->
                context.getBean(ZLinkActorManager.class));
        }
    }

    @Test
    void actorManagerIsBeanWhenSpotNodeAndActorFactoryExist() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(
                SpotNodeWithActorConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            assertInstanceOf(
                ZLinkSpotManager.class,
                context.getBean(ZLinkSpotManager.class));
            assertInstanceOf(
                ZLinkActorManager.class,
                context.getBean(ZLinkActorManager.class));
            assertInstanceOf(
                ZLinkActorDirectory.class,
                context.getBean(ZLinkActorDirectory.class));
        }
    }

    @Test
    void spotPublisherClientIsBeanOnlyWhenPublisherCapabilityExists() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(
                SpotPublisherConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            assertInstanceOf(
                systems.zlink.httpclient.ZLinkFrameworkHttpExecutionTurn.class,
                context.getBean(systems.zlink.httpclient.ZLinkHttpExecutionTurn.class));

            ZLinkSpotPublisherClient publisher =
                context.getBean(ZLinkSpotPublisherClient.class);
            publisher.publish("game", "stage.events", new StageOpened("opened"))
                .submit();
        }
    }

    @Test
    void handlerFactoryCreatesHandlersWithSpringConstructorInjection() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(
                HandlerInjectionConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            ZLinkHandlerActivator handlerFactory = context.getBean(ZLinkHandlerActivator.class);
            InjectedRequestHandler handler =
                (InjectedRequestHandler) handlerFactory.create(InjectedRequestHandler.class);

            String reply = handler.handle("42", requestContext())
                .toCompletableFuture()
                .join();

            assertEquals("profile:42", reply);
        }
    }

    @Test
    void springLifecycleAutoDiscoversSessionPacketHandlersForSessionDispatcher()
        throws Exception {
        FakeZLinkBackendAdapterFactory backendFactory =
            new FakeZLinkBackendAdapterFactory();
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(ZLinkBackendAdapterProvider.class, () -> backendFactory);
            context.register(
                AutoDiscoveredSessionPacketConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            backendFactory.dispatchStreamPacket(
                "auto.session.packet",
                Message.from("{\"value\":\"payload\"}".getBytes(StandardCharsets.UTF_8)),
                ZLinkStreamCodec.JSON);

            context.getBean("sessionPacketHandled", CompletableFuture.class)
                .get(2, TimeUnit.SECONDS);
            assertEquals(1, context.getBean(AtomicInteger.class).get());
        }
    }

    @Test
    void springLifecycleAutoDiscoversSessionPacketHandlersFromApplicationSubpackages()
        throws Exception {
        FakeZLinkBackendAdapterFactory backendFactory =
            new FakeZLinkBackendAdapterFactory();
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(ZLinkBackendAdapterProvider.class, () -> backendFactory);
            context.register(
                AutoDiscoveredSessionPacketSubpackageConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            backendFactory.dispatchStreamPacket(
                "subpackage.session.packet",
                Message.from("{\"value\":\"payload\"}".getBytes(StandardCharsets.UTF_8)),
                ZLinkStreamCodec.JSON);

            context.getBean("sessionPacketHandled", CompletableFuture.class)
                .get(2, TimeUnit.SECONDS);
            assertEquals(1, context.getBean(AtomicInteger.class).get());
        }
    }

    @Test
    void annotatedHandlerGroupHandlesRequestsInsideSpringLifecycle() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.register(
                ScannedHandlerConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            ProfileReply reply = context.getBean(ZLinkClient.class)
                .requestToChannel("profile", new GetProfileRequest("42"))
                .submit(ProfileReply.class)
                .toCompletableFuture()
                .join();

            assertEquals(new ProfileReply("profile:42"), reply);
            assertEquals(
                0,
                context.getBean(AnnotatedInjectedRequestHandler.class).requestCount(),
                "ZLink-managed handlers are created per dispatch, not reused from the root singleton bean");
            assertTrue(context.getBean(ZLinkFrameworkLifecycle.class).isRunning());
        }
    }

    @Test
    void frameworkOwnedHandlerIgnoresRootSingletonAndIsDestroyedWithItsOwner() {
        AnnotatedInjectedRequestHandler.CLOSED.set(0);
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.register(
                ScannedHandlerConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            Object root = context.getBean(AnnotatedInjectedRequestHandler.class);
            try (var owner = new ZLinkHandlerInstanceOwner(
                context.getBean(ZLinkHandlerActivator.class))) {
                Object owned = owner.instance(AnnotatedInjectedRequestHandler.class);
                assertNotSame(root, owned);
            }
            assertEquals(1, AnnotatedInjectedRequestHandler.CLOSED.get());
        }
    }

    @Test
    void handlerAndFilterSharePrototypeDependencyInsideOneActivation() {
        ActivationScopedDependency.reset();
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext(
                     HandlerActivationScopeConfig.class)) {
            var factory = new ZLinkSpringHandlerFactory(
                context.getAutowireCapableBeanFactory());
            ActivationRuntimeService runtimeService =
                new ActivationRuntimeService();
            ZLinkHandlerActivator wrapped =
                ZLinkHandlerActivator.services(factory)
                    .add(ActivationRuntimeService.class, runtimeService);
            ActivationScopedDependency firstDependency;
            try (var owner = new ZLinkHandlerInstanceOwner(wrapped)) {
                ActivationScopedHandler handler =
                    (ActivationScopedHandler) owner.instance(
                        ActivationScopedHandler.class);
                ActivationScopedFilter filter =
                    (ActivationScopedFilter) owner.instance(
                        ActivationScopedFilter.class);
                SecondActivationScopedHandler second =
                    (SecondActivationScopedHandler) owner.instance(
                        SecondActivationScopedHandler.class);
                assertSame(handler.dependency(), filter.dependency());
                assertSame(handler.dependency(), second.dependency());
                assertSame(runtimeService, handler.runtimeService());
                assertSame(runtimeService, filter.runtimeService());
                firstDependency = handler.dependency();
            }
            try (var owner = new ZLinkHandlerInstanceOwner(wrapped)) {
                ActivationScopedHandler handler =
                    (ActivationScopedHandler) owner.instance(
                        ActivationScopedHandler.class);
                assertNotSame(firstDependency, handler.dependency());
            }
            assertEquals(2, ActivationScopedDependency.CLOSED.get());
            assertEquals(0, runtimeService.closed());
        }
    }

    @Test
    void springDestroyCloseIsNotInvokedTwiceForOwnedHandler() {
        SpringManagedCloseHandler.CLOSED.set(0);
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.refresh();
            var factory = new ZLinkSpringHandlerFactory(
                context.getAutowireCapableBeanFactory());
            try (var owner = new ZLinkHandlerInstanceOwner(factory)) {
                owner.instance(SpringManagedCloseHandler.class);
            }
            assertEquals(1, SpringManagedCloseHandler.CLOSED.get());
        }
    }

    @Test
    void scannedHandlersAndCollectionDependenciesAreSpringBeans() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.register(
                AutoRegisteredHandlerConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            ProfileReply reply = context.getBean(ZLinkClient.class)
                .requestToChannel("profile", new DecorateProfileRequest("42"))
                .submit(ProfileReply.class)
                .toCompletableFuture()
                .join();

            assertEquals(new ProfileReply("profile:42:decorated"), reply);
            assertTrue(context.getBean(ZLinkFrameworkLifecycle.class).isRunning());
        }
    }

    @Test
    void scannedHandlersAndSetDependenciesAreSpringBeans() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.register(
                AutoRegisteredSetHandlerConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            ProfileReply reply = context.getBean(ZLinkClient.class)
                .requestToChannel("profile", new DecorateProfileSetRequest("42"))
                .submit(ProfileReply.class)
                .toCompletableFuture()
                .join();

            assertEquals(new ProfileReply("profile:42:decorated"), reply);
            assertTrue(context.getBean(ZLinkFrameworkLifecycle.class).isRunning());
        }
    }

    @Test
    void handlerFiltersAreCreatedThroughSpringDependencyInjection() {
        ActivationScopedDependency.reset();
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.register(
                FilteredHandlerConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            ProfileReply reply = context.getBean(ZLinkClient.class)
                .requestToChannel("profile", new FilteredProfileRequest("42"))
                .submit(ProfileReply.class)
                .toCompletableFuture()
                .join();

            assertEquals(new ProfileReply("profile:42"), reply);
            assertSame(
                ActivationScopedDependency.HANDLER.get(),
                ActivationScopedDependency.FILTER.get());
            assertEquals(1, ActivationScopedDependency.CLOSED.get());
        }
    }

    @Test
    void routeMeshExplicitHandlersAreCreatedThroughSpringDependencyInjection() {
        String sourceEndpoint = tcpEndpoint();
        String targetEndpoint = tcpEndpoint();
        RoutingId sourceRid = RoutingId.from("spring-route-source");
        RoutingId targetRid = RoutingId.from("spring-route-target");

        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(RouteMeshEndpoints.class, () ->
                new RouteMeshEndpoints(sourceEndpoint, targetEndpoint, targetRid));
            context.register(
                RouteMeshHandlerConfig.class,
                ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            try (AnnotationConfigApplicationContext sourceContext =
                     new AnnotationConfigApplicationContext()) {
                sourceContext.registerBean(
                    ZLinkBackendAdapterProvider.class,
                    ZLinkJavaBackendAdapterFactory::new);
                sourceContext.registerBean(
                    ZLinkFrameworkConfigurer.class,
                    () -> options -> { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "route"); channel.enableServer(sourceEndpoint);
                        channel.setRoutingId(sourceRid);
                        channel.enableClient(targetEndpoint); });
                sourceContext.register(
                    SourceRouteMeshConfig.class,
                    ZLinkFrameworkAutoConfiguration.class);
                sourceContext.refresh();

                String reply = sourceContext.getBean(ZLinkRouteClient.class)
                    .requestToNode("route", targetRid, new SpringRouteRequest("hello"))
                    .timeout(Duration.ofSeconds(3))
                    .submit(String.class)
                    .toCompletableFuture()
                    .join();

                assertEquals("route:hello", reply);
            }
        }
    }

    @Test
    void runtimeEventDispatcherIsNotExposedAsSpringBean() {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(TestConfig.class, ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            assertTrue(context.getBeansOfType(ZLinkRuntimeEventDispatcher.class).isEmpty());
        }
    }

    @Test
    void noRuntimeErrorSinkStillOpensNoRawMonitoringSocketMonitor() {
        FakeZLinkBackendAdapterFactory backendFactory =
            new FakeZLinkBackendAdapterFactory();
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(ZLinkBackendAdapterProvider.class, () -> backendFactory);
            context.register(TestConfig.class, ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            // Runtime monitoring registers no socket source, so it opens nothing.
            // The single remaining monitor belongs to the ClientServer client
            // DEALER: spec 12 section 4.4 makes even a manual connection verify
            // ChannelName, server RID and lifecycle generation on the transport,
            // and spec 55 section 3 only marks it ready after that admission.
            assertEquals(
                List.of("monitoring.open.dealer"),
                backendFactory.calls().stream()
                    .filter(call -> call.startsWith("monitoring.open."))
                    .toList());
        }
    }

    @Test
    void locationStoreBeanConfiguresFrameworkLocationRuntime() throws Exception {
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(
                ZLinkBackendAdapterProvider.class,
                FakeZLinkBackendAdapterFactory::new);
            context.register(LocationStoreBeanConfig.class, ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            ZLinkLocationRuntimeQuery query = context
                .getBean(ZLinkFrameworkLifecycle.class)
                .monitoringLocationRuntimeQuery();
            ZLinkLocationRuntimeStatus status = query.getStatus()
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);

            assertTrue(status.ownerLeaseHealthy());
        }
    }

    @Test
    void autoConfigurationAppliesCustomizersBeforeRuntimeStarts() {
        FakeZLinkBackendAdapterFactory backendFactory =
            new FakeZLinkBackendAdapterFactory();
        try (AnnotationConfigApplicationContext context =
                 new AnnotationConfigApplicationContext()) {
            context.registerBean(ZLinkBackendAdapterProvider.class, () -> backendFactory);
            context.register(TestConfig.class, ZLinkFrameworkAutoConfiguration.class);
            context.refresh();

            assertEquals(
                List.of(
                    "factory.channel",
                    "create.context",
                    "factory.monitoring",
                    "create.dealer",
                    "dealer.setChannelName.profile",
                    "monitoring.open.dealer",
                    "create.socketMonitor",
                    "socketMonitor.onEvent",
                    "dealer.connect.inproc://profile-server"),
                backendFactory.calls());
        }

        assertEquals(
            List.of(
                "factory.channel",
                "create.context",
                "factory.monitoring",
                "create.dealer",
                "dealer.setChannelName.profile",
                "monitoring.open.dealer",
                "create.socketMonitor",
                "socketMonitor.onEvent",
                "dealer.connect.inproc://profile-server",
                "close.socketMonitor",
                "close.dealer",
                "close.context"),
            backendFactory.calls());
    }

    @Configuration
    @EnableZLinkFramework
    static class TestConfig {
        @Bean
        ZLinkFrameworkConfigurer profileChannelConfigurer() {
            return options -> options.addClientServerChannel("profile")
                .client()
                .connect("inproc://profile-server");
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class LocationStoreBeanConfig {
        @Bean
        ZLinkInMemoryLocationStore locationStore() {
            return new ZLinkInMemoryLocationStore();
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class StreamCompressionConfig {
        static final ZLinkStreamCompressionCodec COMPRESSION =
            new ZLinkStreamCompressionCodec() {
                @Override
                public byte[] compress(byte[] payload) {
                    return payload;
                }

                @Override
                public byte[] decompress(byte[] payload, int maxDecompressedSize) {
                    return payload;
                }
            };

        @Bean
        ZLinkFrameworkConfigurer streamCompressionConfigurer() {
            return options -> options.configureStreamCompression().use(COMPRESSION);
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class EnabledTestConfig {
        @Bean
        ZLinkFrameworkConfigurer profileChannelConfigurer() {
            return options -> options.addClientServerChannel("profile")
                .client()
                .connect("inproc://profile-server");
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class AutoDiscoveredSessionPacketConfig {
        @Bean
        AtomicInteger sessionPacketCount() {
            return new AtomicInteger();
        }

        @Bean
        CompletableFuture<Void> sessionPacketHandled() {
            return new CompletableFuture<>();
        }

        @Bean
        AutoDiscoveredSessionPacketHandler autoDiscoveredSessionPacketHandler(
            AtomicInteger sessionPacketCount,
            CompletableFuture<Void> sessionPacketHandled) {
            return new AutoDiscoveredSessionPacketHandler(
                sessionPacketCount,
                sessionPacketHandled);
        }

        @Bean
        ZLinkFrameworkConfigurer autoDiscoveredSessionPacketConfigurer() {
            return options -> { var stream = options.addStreamNode("client.stream"); stream.bind("inproc://auto-discovered-session");
                stream.registerSession(AutoDiscoveredPacketSession.class); };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class AutoDiscoveredSessionPacketSubpackageConfig {
        @Bean
        AtomicInteger sessionPacketCount() {
            return new AtomicInteger();
        }

        @Bean
        CompletableFuture<Void> sessionPacketHandled() {
            return new CompletableFuture<>();
        }

        @Bean
        ZLinkFrameworkConfigurer autoDiscoveredSessionPacketConfigurer() {
            return options -> {
                options.addHandlersFromPackageOf(SubpackageSessionPacketAnchor.class);
                var stream = options.addStreamNode("client.stream");
                stream.bind("inproc://auto-discovered-session-subpackage");
                stream.registerSession(SubpackageDiscoveredPacketSession.class);
            };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class SpotNodeConfig {
        @Bean
        ZLinkFrameworkConfigurer spotNodeConfigurer() {
            return options -> { var mesh = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addSpotMesh(options, "game"); { var node = mesh; node.enableRouter("inproc://play-router");
                    node.objects().server().addSpotFactory("GameSpot", GameSpot.class, factory -> factory.disableRelocation()); }; };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class ObjectClientConfig {
        @Bean
        ZLinkFrameworkConfigurer objectClientConfigurer() {
            return options -> {
                var mesh = options.addRouteMesh("game");
                mesh.listen("inproc://object-client");
                mesh.objects().client();
            };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class SpotNodeWithActorConfig {
        @Bean
        ZLinkFrameworkConfigurer spotNodeWithActorConfigurer() {
            return options -> {
                { var mesh = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addSpotMesh(options, "game"); { var node = mesh; node.enableRouter("inproc://play-router");
                        node.objects().server().addSpotFactory("GameSpot", GameSpot.class, factory -> factory.disableRelocation());
                        node.objects().server().addActorFactory("player", PlayerActor.class, PlayerActorFactory.class, factory -> factory.recreateOnRelocation()); }; };
            };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class SpotNodeWithLocationStoreConfig {
        @Bean
        ZLinkInMemoryLocationStore locationStore() {
            return new ZLinkInMemoryLocationStore();
        }

        @Bean
        ZLinkFrameworkConfigurer spotNodeWithLocationStoreConfigurer() {
            return options -> {
                var mesh = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addSpotMesh(options, "game");
                mesh.enableRouter("inproc://play-router");
            };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class PrivateConstructorSpotConfig {
        @Bean
        ZLinkFrameworkConfigurer privateConstructorSpotConfigurer() {
            return options -> { var mesh = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addSpotMesh(options, "game"); { var node = mesh; node.enableRouter("inproc://play-router");
                    node.objects().server().addSpotFactory("PrivateConstructorSpot", PrivateConstructorSpot.class, factory -> factory.disableRelocation()); }; };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class InjectedSpotAndActorConfig {
        @Bean
        HandlerDependency handlerDependency() {
            return new HandlerDependency("spring");
        }

        @Bean
        ZLinkFrameworkConfigurer injectedSpotAndActorConfigurer() {
            return options -> {
                { var mesh = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addSpotMesh(options, "game"); { var node = mesh; node.enableRouter("inproc://play-router");
                        node.objects().server().addSpotFactory("InjectedGameSpot", InjectedGameSpot.class, factory -> factory.disableRelocation());
                        node.objects().server().addActorFactory("player", InjectedPlayerActor.class, InjectedPlayerActorFactory.class, factory -> factory.recreateOnRelocation()); }; };
            };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class SpotPublisherConfig {
        @Bean
        ZLinkFrameworkConfigurer spotPublisherConfigurer() {
            return options -> {
                var mesh = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addSpotMesh(options, "game");
                mesh.enableRouter("inproc://spot-router");
                mesh.enablePubSub("inproc://spot-pub");
                mesh.objects().server().addSpotFactory("GameSpot", GameSpot.class, factory -> factory.disableRelocation());
            };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class HandlerInjectionConfig {
        @Bean
        HandlerDependency handlerDependency() {
            return new HandlerDependency("profile");
        }
    }

    @Configuration
    static class HandlerActivationScopeConfig {
        @Bean
        @Scope(ConfigurableBeanFactory.SCOPE_PROTOTYPE)
        ActivationScopedDependency activationScopedDependency() {
            return new ActivationScopedDependency();
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class ScannedHandlerConfig {
        @Bean
        HandlerDependency handlerDependency() {
            return new HandlerDependency("profile");
        }

        @Bean
        AnnotatedInjectedRequestHandler annotatedInjectedRequestHandler(
            HandlerDependency dependency) {
            return new AnnotatedInjectedRequestHandler(dependency);
        }

        @Bean
        ZLinkFrameworkConfigurer scannedHandlerConfigurer() {
            return options -> {
                options.addHandlersFromPackageOf(ScannedHandlerConfig.class);
                var channel = options.addClientServerChannel("profile");
                channel.client();
                channel.server().listen().addHandlerGroup("spring-scanned");
            };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class AutoRegisteredHandlerConfig {
        @Bean
        HandlerDependency autoRegisteredDependency() {
            return new HandlerDependency("profile");
        }

        @Bean
        ZLinkFrameworkConfigurer autoRegisteredHandlerConfigurer() {
            return options -> {
                options.addHandlersFromPackageOf(AutoRegisteredHandlerConfig.class);
                var channel = options.addClientServerChannel("profile");
                channel.client();
                channel.server().listen().addHandlerGroup("spring-auto-registered");
            };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class AutoRegisteredSetHandlerConfig {
        @Bean
        HandlerDependency autoRegisteredSetDependency() {
            return new HandlerDependency("profile");
        }

        @Bean
        ZLinkFrameworkConfigurer autoRegisteredSetHandlerConfigurer() {
            return options -> {
                options.addHandlersFromPackageOf(AutoRegisteredSetHandlerConfig.class);
                var channel = options.addClientServerChannel("profile");
                channel.client();
                channel.server().listen().addHandlerGroup("spring-auto-registered-set");
            };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class FilteredHandlerConfig {
        @Bean
        HandlerDependency handlerDependency() {
            return new HandlerDependency("profile");
        }

        @Bean
        FilterDependency filterDependency() {
            return new FilterDependency("filter");
        }

        @Bean
        @Scope(ConfigurableBeanFactory.SCOPE_PROTOTYPE)
        ActivationScopedDependency activationScopedDependency() {
            return new ActivationScopedDependency();
        }

        @Bean
        ZLinkFrameworkConfigurer filteredHandlerConfigurer() {
            return options -> {
                options.useFilter(SpringInjectedReplyFilter.class);
                var channel = options.addClientServerChannel("profile");
                channel.client();
                channel.server().listen().addRequestHandler(
                        InjectedProfileRequestHandler.class,
                        FilteredProfileRequest.class,
                        ProfileReply.class);
            };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class RouteMeshHandlerConfig {
        @Bean
        HandlerDependency routeHandlerDependency() {
            return new HandlerDependency("route");
        }

        @Bean
        ZLinkFrameworkConfigurer routeMeshHandlerConfigurer(
            RouteMeshEndpoints endpoints) {
            return options -> { var channel = systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology.addRouteMeshChannel(options, "route"); channel.enableServer(endpoints.targetEndpoint());
                channel.setRoutingId(endpoints.targetRid());
                channel.enableClient(endpoints.sourceEndpoint());
                channel.addRequestHandler(
                    InjectedRouteRequestHandler.class,
                    SpringRouteRequest.class,
                    String.class,
                    "SpringRoute"); };
        }
    }

    @Configuration
    @EnableZLinkFramework
    static class SourceRouteMeshConfig {
    }

    public static final class GameSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public GameSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class InjectedGameSpot implements ZLinkSpot<ZLinkActor> {
        private static final AtomicReference<String> DEPENDENCY_VALUE =
            new AtomicReference<>();
        private final ZLinkSpotContext context;

        public InjectedGameSpot(ZLinkSpotContext context, HandlerDependency dependency) {
            this.context = context;
            DEPENDENCY_VALUE.set(dependency.format("spot"));
        }

        static String dependencyValue() {
            return DEPENDENCY_VALUE.get();
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PrivateConstructorSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        private PrivateConstructorSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerActor implements ZLinkActor {
        private final String actorId;
        private final ZLinkActorContext context;

        PlayerActor(String actorId, ZLinkActorContext context) {
            this.actorId = actorId;
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class InjectedPlayerActor implements ZLinkActor {
        private final String actorId;
        private final ZLinkActorContext context;
        private final String dependencyValue;

        InjectedPlayerActor(
            String actorId,
            ZLinkActorContext context,
            String dependencyValue) {
            this.actorId = actorId;
            this.context = context;
            this.dependencyValue = dependencyValue;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        String dependencyValue() {
            return dependencyValue;
        }
    }

    public static final class PlayerActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(
            ZLinkActorContext context) {
            return CompletableFuture.completedFuture(
                new PlayerActor(context.actorId(), context));
        }
    }

    public static final class InjectedPlayerActorFactory implements ZLinkActorFactory {
        private final HandlerDependency dependency;

        public InjectedPlayerActorFactory(HandlerDependency dependency) {
            this.dependency = dependency;
        }

        @Override
        public CompletionStage<ZLinkActor> create(
            ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new InjectedPlayerActor(
                context.actorId(),
                context,
                dependency.format(context.actorId())));
        }
    }

    public static final class AutoDiscoveredPacketSession implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> dispatcher;

        public AutoDiscoveredPacketSession(
            ZLinkSessionContext context,
            ZLinkSessionPacketDispatcher<ZLinkSessionContext> dispatcher) {
            this.context = context;
            this.dispatcher = dispatcher;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload) {
            return dispatcher.tryHandle(context, dispatch, payload).thenApply(ignored -> null);
        }
    }

    public static final class AutoDiscoveredSessionPacketHandler
        implements ZLinkTypedSessionPacketHandler<ZLinkSessionContext, AutoSessionPacket> {
        private final AtomicInteger count;
        private final CompletableFuture<Void> handled;

        public AutoDiscoveredSessionPacketHandler(
            AtomicInteger count,
            CompletableFuture<Void> handled) {
            this.count = count;
            this.handled = handled;
        }

        @Override
        public Class<AutoSessionPacket> messageType() {
            return AutoSessionPacket.class;
        }

        @Override
        public CompletionStage<Void> handle(
            ZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            AutoSessionPacket payload) {
            count.incrementAndGet();
            handled.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }

    @ZLinkPacket("auto.session.packet")
    public record AutoSessionPacket(String value) {
    }

    static final class HandlerDependency {
        private final String prefix;

        HandlerDependency(String prefix) {
            this.prefix = prefix;
        }

        String format(String value) {
            return prefix + ":" + value;
        }
    }

    static final class FilterDependency {
        private final String prefix;

        FilterDependency(String prefix) {
            this.prefix = prefix;
        }

        String decorate(ProfileReply reply) {
            return prefix + ":" + reply.value();
        }
    }

    public static final class InjectedRequestHandler
        implements ZLinkRequestHandler<String, String> {
        private final HandlerDependency dependency;

        public InjectedRequestHandler(HandlerDependency dependency) {
            this.dependency = dependency;
        }

        @Override
        public CompletionStage<String> handle(
            String request,
            ZLinkMessageContext context) {
            return CompletableFuture.completedFuture(dependency.format(request));
        }
    }

    public static final class InjectedProfileRequestHandler
        implements ZLinkRequestHandler<FilteredProfileRequest, ProfileReply> {
        private final HandlerDependency dependency;
        private final ActivationScopedDependency activationDependency;

        public InjectedProfileRequestHandler(
            HandlerDependency dependency,
            ActivationScopedDependency activationDependency) {
            this.dependency = dependency;
            this.activationDependency = activationDependency;
        }

        @Override
        public CompletionStage<ProfileReply> handle(
            FilteredProfileRequest request,
            ZLinkMessageContext context) {
            ActivationScopedDependency.HANDLER.set(activationDependency);
            return CompletableFuture.completedFuture(
                new ProfileReply(dependency.format(request.profileId())));
        }
    }

    public static final class SpringInjectedReplyFilter implements ZLinkHandlerFilter {
        private final FilterDependency dependency;
        private final ActivationScopedDependency activationDependency;

        public SpringInjectedReplyFilter(
            FilterDependency dependency,
            ActivationScopedDependency activationDependency) {
            this.dependency = dependency;
            this.activationDependency = activationDependency;
        }

        @Override
        public <T> CompletionStage<T> invoke(
            ZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext<T> next) {
            ActivationScopedDependency.FILTER.set(activationDependency);
            return next.invoke();
        }
    }

    @ZLinkHandlerGroup("spring-scanned")
    public static final class AnnotatedInjectedRequestHandler
        implements AutoCloseable {
        static final AtomicInteger CLOSED = new AtomicInteger();
        private final HandlerDependency dependency;
        private int requestCount;

        public AnnotatedInjectedRequestHandler(HandlerDependency dependency) {
            this.dependency = dependency;
        }

        @ZLinkRequest(packetName = "GetProfile")
        public CompletionStage<ProfileReply> handle(GetProfileRequest request) {
            requestCount++;
            return CompletableFuture.completedFuture(
                new ProfileReply(dependency.format(request.profileId())));
        }

        int requestCount() {
            return requestCount;
        }

        @Override
        public void close() {
            CLOSED.incrementAndGet();
        }
    }

    public static final class ActivationScopedDependency
        implements AutoCloseable, DisposableBean {
        static final AtomicInteger CLOSED = new AtomicInteger();
        static final AtomicReference<ActivationScopedDependency> HANDLER =
            new AtomicReference<>();
        static final AtomicReference<ActivationScopedDependency> FILTER =
            new AtomicReference<>();

        static void reset() {
            CLOSED.set(0);
            HANDLER.set(null);
            FILTER.set(null);
        }

        @Override
        public void destroy() {
            close();
        }

        @Override
        public void close() {
            CLOSED.incrementAndGet();
        }
    }

    public static final class ActivationScopedHandler {
        private final ActivationScopedDependency dependency;
        private final ActivationRuntimeService runtimeService;

        public ActivationScopedHandler(
            ActivationScopedDependency dependency,
            ActivationRuntimeService runtimeService) {
            this.dependency = dependency;
            this.runtimeService = runtimeService;
        }

        ActivationScopedDependency dependency() {
            return dependency;
        }

        ActivationRuntimeService runtimeService() {
            return runtimeService;
        }
    }

    public static final class ActivationScopedFilter
        implements ZLinkHandlerFilter {
        private final ActivationScopedDependency dependency;
        private final ActivationRuntimeService runtimeService;

        public ActivationScopedFilter(
            ActivationScopedDependency dependency,
            ActivationRuntimeService runtimeService) {
            this.dependency = dependency;
            this.runtimeService = runtimeService;
        }

        ActivationScopedDependency dependency() {
            return dependency;
        }

        ActivationRuntimeService runtimeService() {
            return runtimeService;
        }

        @Override
        public <T> CompletionStage<T> invoke(
            ZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext<T> next) {
            return next.invoke();
        }
    }

    public static final class SecondActivationScopedHandler {
        private final ActivationScopedDependency dependency;
        private final ActivationRuntimeService runtimeService;

        public SecondActivationScopedHandler(
            ActivationScopedDependency dependency,
            ActivationRuntimeService runtimeService) {
            this.dependency = dependency;
            this.runtimeService = runtimeService;
        }

        ActivationScopedDependency dependency() {
            return dependency;
        }
    }

    public static final class ActivationRuntimeService
        implements AutoCloseable {
        private final AtomicInteger closed = new AtomicInteger();

        int closed() {
            return closed.get();
        }

        @Override
        public void close() {
            closed.incrementAndGet();
        }
    }

    public static final class SpringManagedCloseHandler
        implements AutoCloseable, DisposableBean {
        static final AtomicInteger CLOSED = new AtomicInteger();

        @Override
        public void destroy() {
            close();
        }

        @Override
        public void close() {
            CLOSED.incrementAndGet();
        }
    }

    @ZLinkPacket("StageOpened")
    public record StageOpened(String value) { }

    @ZLinkPacket("GetProfile")
    public record GetProfileRequest(String profileId) { }

    @ZLinkPacket("DecorateProfile")
    public record DecorateProfileRequest(String profileId) { }

    @ZLinkPacket("DecorateProfileSet")
    public record DecorateProfileSetRequest(String profileId) { }

    @ZLinkPacket("FilteredProfile")
    public record FilteredProfileRequest(String profileId) { }

    @ZLinkPacket("SpringRoute")
    public record SpringRouteRequest(String value) { }

    public record ProfileReply(String value) {
    }

    record RouteMeshEndpoints(
        String sourceEndpoint,
        String targetEndpoint,
        RoutingId targetRid) {
    }

    interface ProfileDecorator {
        String decorate(String value);
    }

    public static final class ProfileSuffixDecorator implements ProfileDecorator {
        @Override
        public String decorate(String value) {
            return value + ":decorated";
        }
    }

    @ZLinkHandlerGroup("spring-auto-registered")
    public static final class AutoRegisteredRequestHandler {
        private final HandlerDependency dependency;
        private final List<ProfileDecorator> decorators;

        public AutoRegisteredRequestHandler(
            HandlerDependency dependency,
            List<ProfileDecorator> decorators) {
            this.dependency = dependency;
            this.decorators = decorators;
        }

        @ZLinkRequest(packetName = "DecorateProfile")
        public CompletionStage<ProfileReply> handle(DecorateProfileRequest request) {
            String value = dependency.format(request.profileId());
            for (ProfileDecorator decorator : decorators) {
                value = decorator.decorate(value);
            }
            return CompletableFuture.completedFuture(new ProfileReply(value));
        }
    }

    @ZLinkHandlerGroup("spring-auto-registered-set")
    public static final class AutoRegisteredSetRequestHandler {
        private final HandlerDependency dependency;
        private final Set<ProfileDecorator> decorators;

        public AutoRegisteredSetRequestHandler(
            HandlerDependency dependency,
            Set<ProfileDecorator> decorators) {
            this.dependency = dependency;
            this.decorators = decorators;
        }

        @ZLinkRequest(packetName = "DecorateProfileSet")
        public CompletionStage<ProfileReply> handle(DecorateProfileSetRequest request) {
            String value = dependency.format(request.profileId());
            for (ProfileDecorator decorator : decorators) {
                value = decorator.decorate(value);
            }
            return CompletableFuture.completedFuture(new ProfileReply(value));
        }
    }

    public static final class InjectedRouteRequestHandler
        implements ZLinkRouteRequestHandler<SpringRouteRequest, String> {
        private final HandlerDependency dependency;

        public InjectedRouteRequestHandler(HandlerDependency dependency) {
            this.dependency = dependency;
        }

        @Override
        public CompletionStage<String> handle(
            SpringRouteRequest request,
            ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture(dependency.format(request.value()));
        }
    }

    private static String tcpEndpoint() {
        return "tcp://127.0.0.1:" + nextPort();
    }

    private static int nextPort() {
        for (int attempt = 0; attempt < 200; attempt++) {
            int port = NEXT_PORT.getAndIncrement();
            if (isBindable(port)) {
                return port;
            }
        }
        throw new IllegalStateException("failed to allocate tcp port");
    }

    private static boolean isBindable(int port) {
        try (ServerSocket server = new ServerSocket(port)) {
            server.setReuseAddress(false);
            return true;
        } catch (IOException ignored) {
            return false;
        }
    }

    private static ZLinkMessageContext requestContext() {
        return new ZLinkMessageContext() {
            @Override
            public java.util.Optional<String> meshName() {
                return java.util.Optional.empty();
            }

            @Override
            public java.util.Optional<String> channelName() {
                return java.util.Optional.of("profile");
            }

            @Override
            public String packetName() {
                return "GetProfile";
            }

            @Override
            public java.util.Optional<String> contentType() {
                return java.util.Optional.empty();
            }

            @Override
            public java.util.Map<String, String> metadata() {
                return java.util.Map.of();
            }

            @Override
            public java.util.Optional<String> correlationId() {
                return java.util.Optional.empty();
            }
        };
    }
}
