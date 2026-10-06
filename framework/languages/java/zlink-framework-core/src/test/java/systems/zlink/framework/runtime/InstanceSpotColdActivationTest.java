package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.service.ZLinkInstanceActivationRecoveryCodec;
import systems.zlink.framework.runtime.locations.*;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

final class InstanceSpotColdActivationTest {
    private static CompletableFuture<Void> entered;
    private static CompletableFuture<Void> release;
    private static CompletableFuture<Void> echoHandled;
    private static CompletableFuture<Void> echoRelease;
    private static AtomicInteger initialized;
    private static AtomicInteger handled;
    private static java.util.logging.FileHandler flowLog;

    @org.junit.jupiter.api.BeforeAll
    static void captureFlow() throws Exception {
        java.nio.file.Files.createDirectories(java.nio.file.Path.of("build"));
        flowLog = new java.util.logging.FileHandler("build/instance-cold-activation.flow");
        flowLog.setFormatter(new java.util.logging.SimpleFormatter());
        java.util.logging.Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer")
                .addHandler(flowLog);
    }

    @org.junit.jupiter.api.AfterAll
    static void closeFlow() {
        java.util.logging.Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer")
                .removeHandler(flowLog);
        flowLog.close();
    }

    @Test
    @SuppressWarnings("unchecked")
    void remoteTargetCreatesAndRepliesWhileSourceNeverReserves() throws Exception {
        initialized = new AtomicInteger();
        echoHandled = new CompletableFuture<>();
        echoRelease = new CompletableFuture<>();
        var store = new ZLinkInMemoryLocationStore();
        var recovery = new InMemoryRelocationStore();
        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions
                .configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        targetOptions.addLocationStore(store);
        targetOptions.addRelocationStore(recovery);
        targetOptions
                .addRouteMesh("cold-test")
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "cold", Instance.class, factory -> factory.disableRelocation());
        var sourceOptions = new DefaultZLinkFrameworkOptions();
        sourceOptions.configureInboundDispatch().setMaxQueuedApplicationJobs(1);
        sourceOptions
                .configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        sourceOptions.addLocationStore(store);
        sourceOptions.addRelocationStore(recovery);
        sourceOptions.addRouteMesh("cold-test").listen("tcp://127.0.0.1:0").objects().client();
        try (var target = ZLinkFrameworkRuntimeTestAccess.start(targetOptions);
                var source = ZLinkFrameworkRuntimeTestAccess.start(sourceOptions)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(target)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(source)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            var sourceHost =
                    (systems.zlink.framework.runtime.spots.ZLinkSpotRuntime) source.spotManager();
            var nodesField = sourceHost.getClass().getDeclaredField("routeMeshNodesByName");
            nodesField.setAccessible(true);
            var nodes =
                    (java.util.Map<
                                    String,
                                    systems.zlink.framework.runtime.internal.backend
                                            .ZLinkInternalMeshNode>)
                            nodesField.get(sourceHost);
            var node = nodes.get("cold-test");
            long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            while (node.peers().stream()
                    .noneMatch(
                            peer ->
                                    peer.state()
                                            == systems.zlink.framework.runtime.internal.binding.spot
                                                    .MeshPeerState.ADMITTED)) {
                assertTrue(
                        System.nanoTime() < until,
                        "source and target must complete their existing peer handshake");
                Thread.sleep(10);
            }
            var repositoryField = sourceHost.getClass().getDeclaredField("userSpotLocationStore");
            repositoryField.setAccessible(true);
            var original = repositoryField.get(sourceHost);
            var queueField = sourceHost.getClass().getDeclaredField("applicationJobQueue");
            queueField.setAccessible(true);
            var applicationJobQueue =
                    (systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue)
                            queueField.get(sourceHost);
            var observer =
                    (ZLinkLocationRepository)
                            java.lang.reflect.Proxy.newProxyInstance(
                                    ZLinkLocationRepository.class.getClassLoader(),
                                    new Class<?>[] {ZLinkLocationRepository.class},
                                    (proxy, method, arguments) -> {
                                        if (method.getName().equals("reserve"))
                                            throw new AssertionError(
                                                    "source must not reserve a Missing Instance");
                                        try {
                                            return method.invoke(original, arguments);
                                        } catch (
                                                java.lang.reflect.InvocationTargetException
                                                        failure) {
                                            throw failure.getCause();
                                        }
                                    });
            repositoryField.set(sourceHost, observer);
            try {
                var reply =
                        source.route()
                                .requestToSpot("remote-" + UUID.randomUUID(), new Echo("remote"))
                                .instanceSpot("cold")
                                .timeout(Duration.ofSeconds(5))
                                .submit(Echo.class)
                                .toCompletableFuture();
                echoHandled.get(4, TimeUnit.SECONDS);
                try (var permit =
                        applicationJobQueue
                                .acquire()
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS)) {
                    assertEquals(
                            1, applicationJobQueue.snapshot().effectiveMaxQueuedApplicationJobs());
                    assertEquals(1, applicationJobQueue.snapshot().permitsInUse());
                    echoRelease.complete(null);
                    assertEquals(new Echo("remote"), reply.get(1, TimeUnit.SECONDS));
                    assertEquals(1, applicationJobQueue.snapshot().permitsInUse());
                }
                assertEquals(1, initialized.get());
            } finally {
                echoRelease.complete(null);
                repositoryField.set(sourceHost, original);
            }
        }
    }

    @Test
    void targetCreatesFromDurableEnvelopeAndRetainsRootUntilFirstHandlerCompletes()
            throws Exception {
        handled = new AtomicInteger();
        entered = new CompletableFuture<>();
        release = new CompletableFuture<>();
        initialized = new AtomicInteger();
        var store = new ZLinkInMemoryLocationStore();
        var recovery = new InMemoryRelocationStore();
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(store);
        options.addRelocationStore(recovery);
        options.addRouteMesh("cold-test")
                .listen("inproc://cold-" + UUID.randomUUID())
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "cold", Instance.class, factory -> factory.disableRelocation());
        String spotId = "cold-" + UUID.randomUUID();
        String rootReference;
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            runtime.route()
                    .sendToSpot(spotId, new Initial("first"))
                    .instanceSpot("cold")
                    .submit()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            try {
                entered.get(5, TimeUnit.SECONDS);
                var repository = new ZLinkProviderLocationRepository(store);
                var snapshot =
                        assertInstanceOf(
                                ZLinkAuthoritySnapshot.class,
                                repository
                                        .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false)
                                        .toCompletableFuture()
                                        .get(5, TimeUnit.SECONDS));
                var authority =
                        new ZLinkServiceAuthorityPayloadCodec()
                                .decode(snapshot.payload())
                                .orElseThrow();
                var pointer = authority.activationRecoveryState().orElseThrow();
                rootReference = pointer.reference();
                assertEquals(1, pointer.inboxSequence());
                assertEquals(0, pointer.replayCursor());
                var found =
                        assertInstanceOf(
                                ZLinkRelocationFound.class,
                                recovery.get(pointer.reference(), () -> false)
                                        .toCompletableFuture()
                                        .get(5, TimeUnit.SECONDS));
                var envelope = new ZLinkInstanceActivationRecoveryCodec().decode(found.payload());
                assertEquals(spotId, envelope.targetSpotId());
                assertTrue(envelope.operationHigh() != 0 || envelope.operationLow() != 0);
                assertTrue(envelope.deadlineUnixMs() > 0);
                assertEquals(1, initialized.get());
                var routesField = runtime.getClass().getDeclaredField("authorityRouteRuntime");
                routesField.setAccessible(true);
                var routes = (ZLinkStatefulAuthorityRouteRuntime) routesField.get(runtime);
                routes.reconcile().toCompletableFuture().get(5, TimeUnit.SECONDS);
                assertEquals(
                        1, handled.get(), "live reconciliation must not replay the first record");
            } finally {
                release.complete(null);
            }
            long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            ZLinkAuthoritySnapshot after;
            do {
                after =
                        assertInstanceOf(
                                ZLinkAuthoritySnapshot.class,
                                new ZLinkProviderLocationRepository(store)
                                        .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false)
                                        .toCompletableFuture()
                                        .get(5, TimeUnit.SECONDS));
                if (new ZLinkServiceAuthorityPayloadCodec()
                        .decode(after.payload())
                        .orElseThrow()
                        .activationRecoveryState()
                        .isEmpty()) break;
                assertTrue(
                        System.nanoTime() < until,
                        "handler completion must release the recovery pointer");
                Thread.sleep(10);
            } while (true);
            assertTrue(
                    new ZLinkServiceAuthorityPayloadCodec()
                            .decode(after.payload())
                            .orElseThrow()
                            .activationRecoveryState()
                            .isEmpty());
            assertEquals(1, handled.get());
            while (recovery.get(rootReference, () -> false)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                    instanceof ZLinkRelocationFound) {
                assertTrue(
                        System.nanoTime() < until,
                        "pointer release must delete the activation root");
                Thread.sleep(10);
            }
        }
    }

    public record Initial(String value) {}

    public record Echo(String value) {}

    public static final class Instance implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;

        public Instance(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        public ZLinkInstanceSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addPacket(InitialHandler.class);
            context.handlers().addPacket(EchoHandler.class);
        }

        public CompletionStage<Void> onInitialize() {
            initialized.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class InitialHandler implements ZLinkSpotPacketHandler<Instance, Initial> {
        public CompletionStage<Void> handle(Instance spot, Initial request) {
            assertEquals("first", request.value());
            handled.incrementAndGet();
            entered.complete(null);
            return release;
        }
    }

    public static final class EchoHandler implements ZLinkSpotRequestHandler<Instance, Echo, Echo> {
        public CompletionStage<Echo> handle(Instance spot, Echo request) {
            echoHandled.complete(null);
            return echoRelease.thenApply(ignored -> request);
        }
    }
}
