package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCloseReason;
import systems.zlink.framework.spots.ZLinkSpotClosingContext;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.net.ServerSocket;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class InstanceSpotRuntimeIntegrationTest {
    @Test
    void publicRequestColdActivatesApplicationInstanceOnRemoteNode() throws Exception {
        Zlink.version();
        EchoInstanceSpot.initializations.set(0);
        EchoInstanceSpot.sends.set(0);
        EchoInstanceSpot.closes.set(null);
        EchoInstanceSpot.closeCompleted = new CompletableFuture<>();
        SourceEntrySpot.reset();
        SourceEntrySpot.afterCloseStart = new CompletableFuture<>();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        var store = new ZLinkInMemoryLocationStore();

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        var targetNode = targetOptions.addRouteMesh("game");
        targetNode
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from("instance-target-" + suffix));
        targetNode
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "EchoInstance",
                        EchoInstanceSpot.class,
                        factory -> factory.disableRelocation());

        var sourceOptions = new DefaultZLinkFrameworkOptions();
        sourceOptions.addLocationStore(store);
        sourceOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        var sourceNode = sourceOptions.addRouteMesh("game");
        sourceNode
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from("instance-source-" + suffix));
        sourceNode.objects().client();
        sourceNode.objects().server().addEntrySpot(SourceEntrySpot.class);

        try (ZLinkFrameworkRuntime target =
                        RuntimeTestSupport.startFramework(
                                targetOptions, new ZLinkJavaBackendAdapterFactory());
                ZLinkFrameworkRuntime source =
                        RuntimeTestSupport.startFramework(
                                sourceOptions, new ZLinkJavaBackendAdapterFactory())) {
            SourceEntrySpot.request.set(new Request("echo-" + suffix));
            SourceEntrySpot.start.complete(null);
            assertTrue(EchoInstanceSpot.closeCompleted.get(5, TimeUnit.SECONDS));
            SourceEntrySpot.afterCloseStart.complete(null);
            String reply = SourceEntrySpot.reply.get();

            assertEquals("echo:hello|echo:again|echo:after-close", reply);
            assertEquals(2, EchoInstanceSpot.initializations.get());
            assertEquals(1, EchoInstanceSpot.sends.get());
            assertTrue(EchoInstanceSpot.closes.get());
        }
    }

    @Test
    void authorityMissingIsNotPublishedBeforeLocalInstanceRetires() throws Exception {
        Zlink.version();
        EchoInstanceSpot.initializations.set(0);
        EchoInstanceSpot.generations.clear();
        EchoInstanceSpot.sends.set(0);
        EchoInstanceSpot.closes.set(null);
        SourceEntrySpot.reset();
        SourceEntrySpot.afterCloseStart = new CompletableFuture<>();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        String spotId = "close-order-" + suffix;
        var store = new GatedDeleteStore(new ZLinkInMemoryLocationStore(), spotId);

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        var targetNode = targetOptions.addRouteMesh("game");
        targetNode
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from("close-order-target-" + suffix));
        targetNode
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "EchoInstance",
                        EchoInstanceSpot.class,
                        factory -> factory.disableRelocation());

        var sourceOptions = new DefaultZLinkFrameworkOptions();
        sourceOptions.addLocationStore(store);
        sourceOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        var sourceNode = sourceOptions.addRouteMesh("game");
        sourceNode
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from("close-order-source-" + suffix));
        sourceNode.objects().client();
        sourceNode.objects().server().addEntrySpot(SourceEntrySpot.class);

        try (ZLinkFrameworkRuntime target =
                        RuntimeTestSupport.startFramework(
                                targetOptions, new ZLinkJavaBackendAdapterFactory());
                ZLinkFrameworkRuntime source =
                        RuntimeTestSupport.startFramework(
                                sourceOptions, new ZLinkJavaBackendAdapterFactory())) {
            try {
                SourceEntrySpot.request.set(new Request(spotId));
                SourceEntrySpot.start.complete(null);
                store.deleteApplied.get(5, TimeUnit.SECONDS);

                assertEquals(0, target.activeSpotCount("game"));
                SourceEntrySpot.afterCloseStart.complete(null);
                assertEquals(
                        "echo:hello|echo:again|echo:after-close",
                        SourceEntrySpot.reply.get(5, TimeUnit.SECONDS));
                assertEquals(2, EchoInstanceSpot.initializations.get());
                assertEquals(2, EchoInstanceSpot.generations.size());
                assertNotEquals(
                        EchoInstanceSpot.generations.get(0), EchoInstanceSpot.generations.get(1));
            } finally {
                store.releaseDelete.complete(null);
            }
        }
    }

    @Test
    void failedAuthorityReleaseResumesWithoutRepeatingClosing() throws Exception {
        verifyCloseFailure("release");
    }

    @Test
    void failedClosingCommitKeepsAuthorityAndAdmission() throws Exception {
        verifyCloseFailure("commit");
    }

    @Test
    void failedOnClosingStillReleasesAuthorityOnce() throws Exception {
        verifyCloseFailure("callback");
    }

    @Test
    void failedAdmissionSealResumesAtSeal() throws Exception {
        verifyCloseFailure("seal");
    }

    @Test
    void failedInstanceResourceReleaseResumesAtRelease() throws Exception {
        verifyCloseFailure("resource");
    }

    @Test
    void closingVerdictSurvivesLocalRetirementUntilAuthorityRelease() throws Exception {
        verifyCloseFailure("window");
    }

    @Test
    void closingCommitRejectsBeforeLocalSeal() throws Exception {
        verifyCloseFailure("cas-window");
    }

    @Test
    void directMissingWithoutIntentIsNotFound() throws Exception {
        verifyCloseFailure("missing");
    }

    @Test
    void directCreatingWithoutIntentIsNotFound() throws Exception {
        verifyCloseFailure("creating");
    }

    @Test
    void secondAuthorityReleaseFailureResumesOnSameOwnerReentry() throws Exception {
        verifyCloseFailure("double-release");
    }

    @Test
    void appliedClosingCommitFailureContinuesFromCommittedAuthority() throws Exception {
        verifyCloseFailure("commit-applied-failure");
    }

    @Test
    void inconclusiveClosingCommitResumesOnSameOwnerReentry() throws Exception {
        verifyCloseFailure("commit-applied-read-failure");
    }

    private void verifyCloseFailure(String failureStage) throws Exception {
        boolean failRelease =
                failureStage.equals("release") || failureStage.equals("double-release");
        boolean failCommit = failureStage.equals("commit");
        Zlink.version();
        EchoInstanceSpot.closes.set(null);
        EchoInstanceSpot.closingCalls.set(0);
        EchoInstanceSpot.generations.clear();
        EchoInstanceSpot.closingEntered = new CompletableFuture<>();
        EchoInstanceSpot.closingRelease = CompletableFuture.completedFuture(null);
        EchoInstanceSpot.closingFailure =
                failureStage.equals("callback")
                        ? new IllegalStateException("injected OnClosing failure")
                        : null;
        SourceEntrySpot.reset();
        SourceEntrySpot.closeStart = new CompletableFuture<>();
        if (failCommit
                || failureStage.equals("window")
                || failureStage.equals("cas-window")
                || failureStage.equals("missing")
                || failureStage.equals("creating")) {
            SourceEntrySpot.probeStart = new CompletableFuture<>();
            SourceEntrySpot.probeFailure = new CompletableFuture<>();
            SourceEntrySpot.probeDirect = true;
        }
        if (failureStage.equals("missing")) {
            SourceEntrySpot.closeStart.complete(null);
            SourceEntrySpot.afterCloseStart = new CompletableFuture<>();
        }
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        String spotId = "release-failure-" + suffix;
        String sourceEndpoint;
        String targetEndpoint;
        try (ServerSocket sourcePort = new ServerSocket(0);
                ServerSocket targetPort = new ServerSocket(0)) {
            sourceEndpoint = "tcp://127.0.0.1:" + sourcePort.getLocalPort();
            targetEndpoint = "tcp://127.0.0.1:" + targetPort.getLocalPort();
        }
        var store =
                new GatedDeleteStore(
                        new ZLinkInMemoryLocationStore(),
                        spotId,
                        failureStage.equals("double-release") ? 2 : failRelease ? 1 : 0,
                        failureStage.equals("window"),
                        failureStage.equals("creating"));
        if (!failureStage.equals("window")) {
            store.releaseDelete.complete(null);
        }
        CompletableFuture<String> diagnostic = new CompletableFuture<>();
        java.util.logging.Logger diagnosticLogger =
                java.util.logging.Logger.getLogger(
                        systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer.class
                                .getName());
        java.util.logging.Handler diagnosticHandler =
                new java.util.logging.Handler() {
                    @Override
                    public void publish(java.util.logging.LogRecord record) {
                        String message = record.getMessage();
                        if (message.contains("event_id=zlink.dispatch_error")
                                && message.contains(spotId)) {
                            diagnostic.complete(message);
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        if (failureStage.equals("callback")) {
            diagnosticLogger.addHandler(diagnosticHandler);
        }
        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        if (failureStage.equals("callback")) {
            targetOptions
                    .configureDispatch()
                    .messageFlow(
                            systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        }
        targetOptions
                .addRouteMesh("game")
                .listen(targetEndpoint)
                .setRoutingId(RoutingId.from("release-failure-target-" + suffix))
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "EchoInstance",
                        EchoInstanceSpot.class,
                        factory -> factory.disableRelocation());
        var sourceOptions = new DefaultZLinkFrameworkOptions();
        sourceOptions.addLocationStore(store);
        sourceOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        var sourceNode = sourceOptions.addRouteMesh("game");
        sourceNode
                .listen(sourceEndpoint)
                .setRoutingId(RoutingId.from("release-failure-source-" + suffix));
        sourceNode.objects().client();
        sourceNode.objects().server().addEntrySpot(SourceEntrySpot.class);

        try (ZLinkFrameworkRuntime target =
                        RuntimeTestSupport.startFramework(
                                targetOptions, new ZLinkJavaBackendAdapterFactory());
                ZLinkFrameworkRuntime source =
                        RuntimeTestSupport.startFramework(
                                sourceOptions, new ZLinkJavaBackendAdapterFactory())) {
            SourceEntrySpot.request.set(new Request(spotId));
            SourceEntrySpot.start.complete(null);
            try {
                if (failureStage.equals("creating") || failureStage.equals("missing")) {
                    if (failureStage.equals("creating")) {
                        store.readyPending.get(5, TimeUnit.SECONDS);
                    } else {
                        store.deleteApplied.get(5, TimeUnit.SECONDS);
                    }
                    SourceEntrySpot.probeStart.complete(null);
                    Throwable notFound = SourceEntrySpot.probeFailure.get(5, TimeUnit.SECONDS);
                    assertTrue(notFound instanceof ZLinkFrameworkException);
                    assertEquals(
                            ZLinkFrameworkErrorKind.NOT_FOUND,
                            ((ZLinkFrameworkException) notFound).kind());
                    store.releaseReady.complete(null);
                    SourceEntrySpot.afterCloseStart.complete(null);
                    if (failureStage.equals("creating")) {
                        SourceEntrySpot.closeStart.complete(null);
                    }
                    assertEquals(
                            "echo:hello|echo:again|echo:after-close",
                            SourceEntrySpot.reply.get(5, TimeUnit.SECONDS));
                    return;
                }
                CompletableFuture.anyOf(SourceEntrySpot.beforeClose, SourceEntrySpot.reply)
                        .get(5, TimeUnit.SECONDS);
                assertTrue(SourceEntrySpot.beforeClose.isDone());
                if (failCommit) {
                    store.failNextAuthorityPut.set(true);
                } else if (failureStage.equals("commit-applied-failure")
                        || failureStage.equals("commit-applied-read-failure")) {
                    store.failNextAuthorityPutAfterApply.set(true);
                    store.failReadAfterAppliedPut.set(
                            failureStage.equals("commit-applied-read-failure"));
                } else if (failureStage.equals("cas-window")) {
                    store.holdNextAuthorityPutAfterApply.set(true);
                }
                var field = ZLinkFrameworkRuntime.class.getDeclaredField("spots");
                field.setAccessible(true);
                var spots = field.get(target);
                AtomicInteger completedBackendOperation =
                        failureStage.equals("seal") || failureStage.equals("resource")
                                ? installBackendFailure(
                                        spots,
                                        spotId,
                                        failureStage.equals("seal")
                                                ? "sealSpotAdmission"
                                                : "closeInstanceSpot")
                                : null;
                var close =
                        spots.getClass()
                                .getDeclaredMethod("closeInstanceSpot", String.class, long.class);
                close.setAccessible(true);
                @SuppressWarnings("unchecked")
                CompletionStage<Boolean> closing =
                        (CompletionStage<Boolean>)
                                close.invoke(
                                        spots, spotId, EchoInstanceSpot.generations.getFirst());
                if (failureStage.equals("double-release")) {
                    store.deleteFailed.get(5, TimeUnit.SECONDS);
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> closing.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    assertEquals(0, target.activeSpotCount("game"));
                    @SuppressWarnings("unchecked")
                    CompletionStage<Boolean> second =
                            (CompletionStage<Boolean>)
                                    close.invoke(
                                            spots, spotId, EchoInstanceSpot.generations.getFirst());
                    store.deleteFailedTwice.get(5, TimeUnit.SECONDS);
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> second.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    @SuppressWarnings("unchecked")
                    CompletionStage<Boolean> resumed =
                            (CompletionStage<Boolean>)
                                    close.invoke(
                                            spots, spotId, EchoInstanceSpot.generations.getFirst());
                    assertTrue(resumed.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    store.deleteApplied.get(5, TimeUnit.SECONDS);
                    assertEquals(1, EchoInstanceSpot.closingCalls.get());
                    return;
                }
                if (failureStage.equals("commit-applied-read-failure")) {
                    store.putAppliedThenFailed.get(5, TimeUnit.SECONDS);
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> closing.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    assertEquals(1, target.activeSpotCount("game"));
                    @SuppressWarnings("unchecked")
                    CompletionStage<Boolean> resumed =
                            (CompletionStage<Boolean>)
                                    close.invoke(
                                            spots, spotId, EchoInstanceSpot.generations.getFirst());
                    assertTrue(resumed.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    store.deleteApplied.get(5, TimeUnit.SECONDS);
                    assertEquals(1, EchoInstanceSpot.closingCalls.get());
                    return;
                }
                if (failureStage.equals("cas-window")) {
                    store.closingApplied.get(5, TimeUnit.SECONDS);
                    assertEquals(1, target.activeSpotCount("game"));
                    SourceEntrySpot.probeStart.complete(null);
                    Throwable rejected = SourceEntrySpot.probeFailure.get(5, TimeUnit.SECONDS);
                    assertTrue(rejected instanceof ZLinkFrameworkException);
                    assertEquals(
                            ZLinkFrameworkErrorKind.REJECTED,
                            ((ZLinkFrameworkException) rejected).kind());
                    store.releaseClosing.complete(null);
                    assertTrue(closing.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    assertEquals(1, EchoInstanceSpot.closingCalls.get());
                    return;
                }
                if (failureStage.equals("window")) {
                    store.deleteAttempted.get(5, TimeUnit.SECONDS);
                    assertEquals(0, target.activeSpotCount("game"));
                    SourceEntrySpot.probeStart.complete(null);
                    Throwable rejected = SourceEntrySpot.probeFailure.get(5, TimeUnit.SECONDS);
                    assertTrue(rejected instanceof ZLinkFrameworkException);
                    assertEquals(
                            ZLinkFrameworkErrorKind.REJECTED,
                            ((ZLinkFrameworkException) rejected).kind());
                    store.releaseDelete.complete(null);
                    assertTrue(closing.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    assertEquals(1, EchoInstanceSpot.closingCalls.get());
                    return;
                }
                if (failRelease) {
                    store.deleteFailed.get(5, TimeUnit.SECONDS);
                } else if (failCommit) {
                    store.putFailed.get(5, TimeUnit.SECONDS);
                } else if (failureStage.equals("commit-applied-failure")) {
                    store.putAppliedThenFailed.get(5, TimeUnit.SECONDS);
                }
                if (failureStage.equals("callback")) {
                    // Spot address messaging §7: OnClosing failure is diagnostic; Close continues.
                    assertTrue(closing.toCompletableFuture().get(5, TimeUnit.SECONDS));
                } else {
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> closing.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    if (!failCommit) {
                        @SuppressWarnings("unchecked")
                        CompletionStage<Boolean> resumed =
                                (CompletionStage<Boolean>)
                                        close.invoke(
                                                spots,
                                                spotId,
                                                EchoInstanceSpot.generations.getFirst());
                        assertTrue(resumed.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    }
                }
                if (!failCommit) {
                    store.deleteApplied.get(5, TimeUnit.SECONDS);
                    assertEquals(0, target.activeSpotCount("game"));
                    assertEquals(1, EchoInstanceSpot.closingCalls.get());
                    if (completedBackendOperation != null) {
                        assertEquals(1, completedBackendOperation.get());
                    }
                    if (failureStage.equals("callback")) {
                        assertTrue(diagnostic.get(5, TimeUnit.SECONDS).contains(spotId));
                    }
                } else {
                    assertEquals(1, target.activeSpotCount("game"));
                    assertEquals(0, EchoInstanceSpot.closingCalls.get());
                    SourceEntrySpot.probeStart.complete(null);
                    assertTrue(
                            SourceEntrySpot.probeFailure.get(5, TimeUnit.SECONDS)
                                    instanceof AssertionError);
                }
            } finally {
                store.releaseDelete.complete(null);
                store.releaseClosing.complete(null);
                store.releaseReady.complete(null);
                SourceEntrySpot.closeStart.complete(null);
                SourceEntrySpot.afterCloseStart.complete(null);
                EchoInstanceSpot.closingFailure = null;
                diagnosticLogger.removeHandler(diagnosticHandler);
            }
        }
    }

    private static AtomicInteger installBackendFailure(
            Object spots, String spotId, String operation) throws Exception {
        var activations = spots.getClass().getDeclaredField("instanceSpotActivations");
        activations.setAccessible(true);
        Object activation = ((java.util.Map<?, ?>) activations.get(spots)).get(spotId);
        var backendField = activation.getClass().getSuperclass().getDeclaredField("backendSpot");
        backendField.setAccessible(true);
        Object backend = backendField.get(activation);
        AtomicBoolean failOnce = new AtomicBoolean(true);
        AtomicInteger completed = new AtomicInteger();
        Object proxy =
                java.lang.reflect.Proxy.newProxyInstance(
                        backend.getClass().getClassLoader(),
                        new Class<?>[] {
                            systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot.class
                        },
                        (ignored, method, args) -> {
                            if (method.getName().equals(operation)
                                    && failOnce.compareAndSet(true, false)) {
                                throw new IllegalStateException(
                                        "injected " + operation + " failure");
                            }
                            try {
                                Object result = method.invoke(backend, args);
                                if (method.getName().equals(operation)) {
                                    completed.incrementAndGet();
                                }
                                return result;
                            } catch (java.lang.reflect.InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        });
        backendField.set(activation, proxy);
        var contextField = activation.getClass().getSuperclass().getDeclaredField("context");
        contextField.setAccessible(true);
        Object context = contextField.get(activation);
        var contextBackend = context.getClass().getDeclaredField("backendSpot");
        contextBackend.setAccessible(true);
        contextBackend.set(context, proxy);
        return completed;
    }

    @Test
    void publicRequestToClosingInstanceIsRejected() throws Exception {
        verifyClosingInstanceVerdict(false, ZLinkFrameworkErrorKind.REJECTED);
    }

    @Test
    void publicRequestToDrainingClosingInstanceIsShuttingDown() throws Exception {
        verifyClosingInstanceVerdict(true, ZLinkFrameworkErrorKind.SHUTTING_DOWN);
    }

    private void verifyClosingInstanceVerdict(boolean drain, ZLinkFrameworkErrorKind expectedKind)
            throws Exception {
        Zlink.version();
        EchoInstanceSpot.closes.set(null);
        EchoInstanceSpot.closingEntered = new CompletableFuture<>();
        EchoInstanceSpot.closingRelease = new CompletableFuture<>();
        SourceEntrySpot.reset();
        SourceEntrySpot.probeStart = new CompletableFuture<>();
        SourceEntrySpot.probeFailure = new CompletableFuture<>();
        SourceEntrySpot.afterCloseStart = new CompletableFuture<>();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        String spotId = "closing-verdict-" + suffix;
        String targetEndpoint;
        String sourceEndpoint;
        try (ServerSocket targetPort = new ServerSocket(0);
                ServerSocket sourcePort = new ServerSocket(0)) {
            targetEndpoint = "tcp://127.0.0.1:" + targetPort.getLocalPort();
            sourceEndpoint = "tcp://127.0.0.1:" + sourcePort.getLocalPort();
        }
        var store = new ZLinkInMemoryLocationStore();

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        targetOptions
                .addRouteMesh("game")
                .listen(targetEndpoint)
                .setRoutingId(RoutingId.from("closing-verdict-target-" + suffix))
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "EchoInstance",
                        EchoInstanceSpot.class,
                        factory -> factory.disableRelocation());

        var sourceOptions = new DefaultZLinkFrameworkOptions();
        sourceOptions.addLocationStore(store);
        sourceOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        var sourceNode = sourceOptions.addRouteMesh("game");
        sourceNode
                .listen(sourceEndpoint)
                .setRoutingId(RoutingId.from("closing-verdict-source-" + suffix));
        sourceNode.objects().client();
        sourceNode.objects().server().addEntrySpot(SourceEntrySpot.class);

        try (ZLinkFrameworkRuntime target =
                        RuntimeTestSupport.startFramework(
                                targetOptions, new ZLinkJavaBackendAdapterFactory());
                ZLinkFrameworkRuntime source =
                        RuntimeTestSupport.startFramework(
                                sourceOptions, new ZLinkJavaBackendAdapterFactory())) {
            try {
                SourceEntrySpot.request.set(new Request(spotId));
                SourceEntrySpot.start.complete(null);
                EchoInstanceSpot.closingEntered.get(5, TimeUnit.SECONDS);
                if (drain) {
                    var field = ZLinkFrameworkRuntime.class.getDeclaredField("spots");
                    field.setAccessible(true);
                    ((systems.zlink.framework.runtime.spots.ZLinkSpotRuntime) field.get(target))
                            .beginDrain()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
                }
                SourceEntrySpot.probeStart.complete(null);
                Throwable failure = SourceEntrySpot.probeFailure.get(5, TimeUnit.SECONDS);
                assertTrue(failure instanceof ZLinkFrameworkException);
                assertEquals(expectedKind, ((ZLinkFrameworkException) failure).kind());
            } finally {
                EchoInstanceSpot.closingRelease.complete(null);
                SourceEntrySpot.afterCloseStart.complete(null);
            }
            if (drain) {
                var failure =
                        assertThrows(
                                java.util.concurrent.ExecutionException.class,
                                () -> SourceEntrySpot.reply.get(5, TimeUnit.SECONDS));
                assertTrue(failure.getCause() instanceof ZLinkFrameworkException);
                assertEquals(
                        ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                        ((ZLinkFrameworkException) failure.getCause()).kind());
            } else {
                assertEquals(
                        "echo:hello|echo:again|echo:after-close",
                        SourceEntrySpot.reply.get(5, TimeUnit.SECONDS));
            }
        }
    }

    @Test
    void publicRequestReactivatesInstanceSpotAfterIdleEviction() throws Exception {
        Zlink.version();
        EchoInstanceSpot.initializations.set(0);
        EchoInstanceSpot.sends.set(0);
        EchoInstanceSpot.closes.set(null);
        EchoInstanceSpot.closeReason.set(null);
        EchoInstanceSpot.idleEvicted = new CompletableFuture<>();
        IdleSourceEntrySpot.reset();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        var store = new ZLinkInMemoryLocationStore();
        String spotId = "idle-" + suffix;
        IdleSourceEntrySpot.store = store;
        IdleSourceEntrySpot.spotId = spotId;

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        var targetNode = targetOptions.addRouteMesh("game");
        targetNode
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from("instance-idle-target-" + suffix))
                .setInstanceSpotIdleTimeout(Duration.ofMillis(100));
        targetNode
                .objects()
                .server()
                .addInstanceSpotFactory(
                        "EchoInstance",
                        EchoInstanceSpot.class,
                        factory -> factory.disableRelocation());

        var sourceOptions = new DefaultZLinkFrameworkOptions();
        sourceOptions.addLocationStore(store);
        sourceOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        var sourceNode = sourceOptions.addRouteMesh("game");
        sourceNode
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from("instance-idle-source-" + suffix));
        sourceNode.objects().client();
        sourceNode.objects().server().addEntrySpot(IdleSourceEntrySpot.class);

        try (ZLinkFrameworkRuntime target =
                        RuntimeTestSupport.startFramework(
                                targetOptions, new ZLinkJavaBackendAdapterFactory());
                ZLinkFrameworkRuntime source =
                        RuntimeTestSupport.startFramework(
                                sourceOptions, new ZLinkJavaBackendAdapterFactory())) {
            IdleSourceEntrySpot.request.set(new Request(spotId));
            IdleSourceEntrySpot.start.complete(null);
            String reply = IdleSourceEntrySpot.reply.get(10, TimeUnit.SECONDS);

            assertEquals("echo:hello|echo:after-idle", reply);
            assertEquals(2, EchoInstanceSpot.initializations.get());
            assertEquals(ZLinkSpotCloseReason.IDLE_EVICTED, EchoInstanceSpot.closeReason.get());
        }
    }

    private record Request(String spotId) {}

    private record Warmup(String value) {}

    private record CloseInstance() {}

    public static final class SourceEntrySpot implements ZLinkEntrySpot<ZLinkActor> {
        static CompletableFuture<Void> start;
        static CompletableFuture<Void> beforeClose;
        static CompletableFuture<Void> closeStart;
        static CompletableFuture<Void> afterCloseStart;
        static CompletableFuture<Void> probeStart;
        static CompletableFuture<Throwable> probeFailure;
        static boolean probeDirect;
        static AtomicReference<Request> request;
        static CompletableFuture<String> reply;
        private final ZLinkEntrySpotContext context;

        public SourceEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        static void reset() {
            start = new CompletableFuture<>();
            beforeClose = new CompletableFuture<>();
            closeStart = CompletableFuture.completedFuture(null);
            afterCloseStart = CompletableFuture.completedFuture(null);
            probeStart = null;
            probeFailure = null;
            probeDirect = false;
            request = new AtomicReference<>();
            reply = new CompletableFuture<>();
        }

        @Override
        public ZLinkEntrySpotContext context() {
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

        @Override
        public CompletionStage<Void> onInitialize() {
            return start.thenCompose(
                    ignored -> {
                        CompletionStage<Void> warmup =
                                context.outbound()
                                        .sendToSpot(request.get().spotId(), new Warmup("warmup"))
                                        .instanceSpot("EchoInstance")
                                        .inMesh("game")
                                        .submit();
                        CompletionStage<String> first =
                                warmup.thenCompose(
                                        sendCompleted ->
                                                context.outbound()
                                                        .requestToSpot(
                                                                request.get().spotId(), "hello")
                                                        .instanceSpot("EchoInstance")
                                                        .inMesh("game")
                                                        .timeout(Duration.ofSeconds(5))
                                                        .submit(String.class));
                        CompletionStage<String> firstAndSecond =
                                first.thenCompose(
                                        firstValue -> {
                                            return context.outbound()
                                                    .requestToSpot(request.get().spotId(), "again")
                                                    .instanceSpot()
                                                    .inMesh("game")
                                                    .timeout(Duration.ofSeconds(5))
                                                    .submit(String.class)
                                                    .thenApply(
                                                            secondValue ->
                                                                    firstValue + "|" + secondValue);
                                        });
                        CompletionStage<String> beforeAfterClose =
                                firstAndSecond.thenCompose(
                                        value -> {
                                            beforeClose.complete(null);
                                            return closeStart.thenCompose(
                                                    closeAllowed ->
                                                            context.outbound()
                                                                    .sendToSpot(
                                                                            request.get().spotId(),
                                                                            new CloseInstance())
                                                                    .instanceSpot()
                                                                    .inMesh("game")
                                                                    .submit()
                                                                    .thenApply(
                                                                            ignoredClose -> value));
                                        });
                        if (probeStart != null) {
                            probeStart
                                    .thenCompose(
                                            probeStarted -> {
                                                var outbound =
                                                        context.outbound()
                                                                .requestToSpot(
                                                                        request.get().spotId(),
                                                                        "during-close");
                                                return probeDirect
                                                        ? outbound.inMesh("game")
                                                                .timeout(Duration.ofSeconds(5))
                                                                .submit(String.class)
                                                        : outbound.instanceSpot()
                                                                .inMesh("game")
                                                                .timeout(Duration.ofSeconds(5))
                                                                .submit(String.class);
                                            })
                                    .whenComplete(
                                            (value, failure) ->
                                                    probeFailure.complete(
                                                            failure == null
                                                                    ? new AssertionError(
                                                                            "Closing request was admitted")
                                                                    : failure
                                                                                    instanceof
                                                                                    CompletionException
                                                                                            wrapped
                                                                            ? wrapped.getCause()
                                                                            : failure));
                        }
                        CompletableFuture<String> completion =
                                beforeAfterClose
                                        .thenCompose(
                                                value ->
                                                        afterCloseStart.thenCompose(
                                                                afterCloseAllowed ->
                                                                        context.outbound()
                                                                                .requestToSpot(
                                                                                        request.get()
                                                                                                .spotId(),
                                                                                        "after-close")
                                                                                .instanceSpot()
                                                                                .inMesh("game")
                                                                                .timeout(
                                                                                        Duration
                                                                                                .ofSeconds(
                                                                                                        5))
                                                                                .submit(
                                                                                        String
                                                                                                .class)
                                                                                .thenApply(
                                                                                        after ->
                                                                                                value
                                                                                                        + "|"
                                                                                                        + after)))
                                        .toCompletableFuture();
                        completion.whenComplete(
                                (value, failure) -> {
                                    if (failure == null) {
                                        reply.complete(value);
                                    } else {
                                        reply.completeExceptionally(failure);
                                    }
                                });
                        return completion.thenApply(value -> null);
                    });
        }
    }

    public static final class IdleSourceEntrySpot implements ZLinkEntrySpot<ZLinkActor> {
        static CompletableFuture<Void> start;
        static AtomicReference<Request> request;
        static CompletableFuture<String> reply;
        static ZLinkInMemoryLocationStore store;
        static String spotId;
        private final ZLinkEntrySpotContext context;

        public IdleSourceEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        static void reset() {
            start = new CompletableFuture<>();
            request = new AtomicReference<>();
            reply = new CompletableFuture<>();
        }

        @Override
        public ZLinkEntrySpotContext context() {
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

        @Override
        public CompletionStage<Void> onInitialize() {
            return start.thenCompose(
                            ignored ->
                                    context.outbound()
                                            .requestToSpot(spotId, "hello")
                                            .instanceSpot("EchoInstance")
                                            .inMesh("game")
                                            .timeout(Duration.ofSeconds(5))
                                            .submit(String.class))
                    .thenCompose(
                            first -> {
                                long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
                                return EchoInstanceSpot.idleEvicted
                                        .thenCompose(
                                                ignored ->
                                                        awaitAuthorityMissing(
                                                                store, spotId, deadline))
                                        .thenComposeAsync(
                                                ignored ->
                                                        context.outbound()
                                                                .requestToSpot(
                                                                        spotId,
                                                                        "ordinary-after-idle")
                                                                .inMesh("game")
                                                                .timeout(Duration.ofSeconds(5))
                                                                .submit(String.class)
                                                                .handle(
                                                                        (value, failure) -> {
                                                                            if (failure == null) {
                                                                                throw new AssertionError(
                                                                                        "ordinary"
                                                                                                + " request"
                                                                                                + " succeeded"
                                                                                                + " after"
                                                                                                + " idle"
                                                                                                + " eviction");
                                                                            }
                                                                            Throwable cause =
                                                                                    unwrap(failure);
                                                                            assertTrue(
                                                                                    cause
                                                                                            instanceof
                                                                                            ZLinkFrameworkException,
                                                                                    "ordinary"
                                                                                            + " request"
                                                                                            + " failure"
                                                                                            + " was not"
                                                                                            + " typed: "
                                                                                            + cause);
                                                                            assertEquals(
                                                                                    ZLinkFrameworkErrorKind
                                                                                            .NOT_FOUND,
                                                                                    ((ZLinkFrameworkException)
                                                                                                    cause)
                                                                                            .kind());
                                                                            return (Void) null;
                                                                        }),
                                                CompletableFuture.delayedExecutor(
                                                        1, TimeUnit.MILLISECONDS))
                                        .thenComposeAsync(
                                                ignored ->
                                                        context.outbound()
                                                                .requestToSpot(spotId, "after-idle")
                                                                .instanceSpot("EchoInstance")
                                                                .inMesh("game")
                                                                .timeout(Duration.ofSeconds(5))
                                                                .submit(String.class),
                                                CompletableFuture.delayedExecutor(
                                                        1, TimeUnit.MILLISECONDS))
                                        .thenApply(after -> first + "|" + after);
                            })
                    .whenComplete(
                            (value, failure) -> {
                                if (failure == null) {
                                    reply.complete(value);
                                } else {
                                    reply.completeExceptionally(failure);
                                }
                            })
                    .thenApply(ignored -> null);
        }
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable cause = failure;
        while (cause instanceof CompletionException && cause.getCause() != null) {
            cause = cause.getCause();
        }
        return cause;
    }

    private static CompletionStage<Void> awaitAuthorityMissing(
            ZLinkInMemoryLocationStore store, String spotId, long deadlineNanos) {
        return store.read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false)
                .thenCompose(
                        read -> {
                            if (read
                                    instanceof
                                    systems.zlink.framework.runtime.internal.locations
                                            .ZLinkAuthorityMissing) {
                                return CompletableFuture.completedFuture(null);
                            }
                            if (System.nanoTime() >= deadlineNanos) {
                                return CompletableFuture.failedFuture(
                                        new AssertionError(
                                                "Instance Spot authority was not deleted"));
                            }
                            return CompletableFuture.supplyAsync(
                                            () -> (Void) null,
                                            CompletableFuture.delayedExecutor(
                                                    10, TimeUnit.MILLISECONDS))
                                    .thenCompose(
                                            ignored ->
                                                    awaitAuthorityMissing(
                                                            store, spotId, deadlineNanos));
                        });
    }

    private static final class GatedDeleteStore implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate;
        private final String authorityKey;
        private final AtomicInteger deleteFailuresRemaining;
        private final boolean holdDeleteBeforeApply;
        private final boolean holdReadyBeforeApply;
        private final AtomicBoolean failNextAuthorityPut = new AtomicBoolean();
        private final AtomicBoolean failNextAuthorityPutAfterApply = new AtomicBoolean();
        private final AtomicBoolean failReadAfterAppliedPut = new AtomicBoolean();
        private final AtomicBoolean pendingFailedRead = new AtomicBoolean();
        private final AtomicBoolean holdNextAuthorityPutAfterApply = new AtomicBoolean();
        private final AtomicBoolean intercepted = new AtomicBoolean();
        private final CompletableFuture<Void> deleteApplied = new CompletableFuture<>();
        private final CompletableFuture<Void> deleteAttempted = new CompletableFuture<>();
        private final CompletableFuture<Void> deleteFailed = new CompletableFuture<>();
        private final CompletableFuture<Void> deleteFailedTwice = new CompletableFuture<>();
        private final CompletableFuture<Void> putFailed = new CompletableFuture<>();
        private final CompletableFuture<Void> putAppliedThenFailed = new CompletableFuture<>();
        private final CompletableFuture<Void> closingApplied = new CompletableFuture<>();
        private final CompletableFuture<Void> releaseClosing = new CompletableFuture<>();
        private final CompletableFuture<Void> readyPending = new CompletableFuture<>();
        private final CompletableFuture<Void> releaseReady = new CompletableFuture<>();
        private final AtomicInteger authorityPutCount = new AtomicInteger();
        private final CompletableFuture<Void> releaseDelete = new CompletableFuture<>();

        private GatedDeleteStore(ZLinkLocationStore delegate, String spotId) {
            this(delegate, spotId, 0, false, false);
        }

        private GatedDeleteStore(
                ZLinkLocationStore delegate, String spotId, boolean failFirstDelete) {
            this(delegate, spotId, failFirstDelete ? 1 : 0, false, false);
        }

        private GatedDeleteStore(
                ZLinkLocationStore delegate,
                String spotId,
                int deleteFailures,
                boolean holdDeleteBeforeApply,
                boolean holdReadyBeforeApply) {
            this.delegate = delegate;
            this.authorityKey = "authority\0spot\0" + spotId;
            this.deleteFailuresRemaining = new AtomicInteger(deleteFailures);
            this.holdDeleteBeforeApply = holdDeleteBeforeApply;
            this.holdReadyBeforeApply = holdReadyBeforeApply;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                systems.zlink.framework.locationprovider.ZLinkStoreKey key,
                ZLinkStoreCancellation cancellation) {
            if (authorityKey.equals(key.value()) && pendingFailedRead.compareAndSet(true, false)) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("injected Closing reconciliation read failure"));
            }
            return delegate.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            boolean targetPut =
                    request.mutations().stream()
                            .filter(
                                    systems.zlink.framework.locationprovider.ZLinkStorePut.class
                                            ::isInstance)
                            .map(systems.zlink.framework.locationprovider.ZLinkStorePut.class::cast)
                            .anyMatch(put -> authorityKey.equals(put.key().value()));
            if (targetPut && holdReadyBeforeApply && authorityPutCount.incrementAndGet() == 2) {
                readyPending.complete(null);
                return releaseReady.thenCompose(ignored -> delegate.write(request, cancellation));
            }
            if (targetPut && failNextAuthorityPut.compareAndSet(true, false)) {
                putFailed.complete(null);
                return CompletableFuture.failedFuture(
                        new IllegalStateException("injected Closing commit failure"));
            }
            if (targetPut && failNextAuthorityPutAfterApply.compareAndSet(true, false)) {
                return delegate.write(request, cancellation)
                        .thenCompose(
                                ignored -> {
                                    if (failReadAfterAppliedPut.compareAndSet(true, false)) {
                                        pendingFailedRead.set(true);
                                    }
                                    putAppliedThenFailed.complete(null);
                                    return CompletableFuture.failedFuture(
                                            new IllegalStateException(
                                                    "injected applied Closing failure"));
                                });
            }
            if (targetPut && holdNextAuthorityPutAfterApply.compareAndSet(true, false)) {
                return delegate.write(request, cancellation)
                        .thenCompose(
                                result -> {
                                    closingApplied.complete(null);
                                    return releaseClosing.thenApply(ignored -> result);
                                });
            }
            boolean targetDelete =
                    request.mutations().stream()
                            .filter(ZLinkStoreDelete.class::isInstance)
                            .map(ZLinkStoreDelete.class::cast)
                            .anyMatch(delete -> authorityKey.equals(delete.key().value()));
            if (targetDelete && holdDeleteBeforeApply && intercepted.compareAndSet(false, true)) {
                deleteAttempted.complete(null);
                return releaseDelete
                        .thenCompose(ignored -> delegate.write(request, cancellation))
                        .thenApply(
                                result -> {
                                    deleteApplied.complete(null);
                                    return result;
                                });
            }
            int failuresBefore =
                    targetDelete
                            ? deleteFailuresRemaining.getAndUpdate(
                                    remaining -> Math.max(0, remaining - 1))
                            : 0;
            if (failuresBefore > 0) {
                deleteFailed.complete(null);
                if (failuresBefore == 1) {
                    deleteFailedTwice.complete(null);
                }
                return CompletableFuture.failedFuture(
                        new IllegalStateException("injected authority release failure"));
            }
            CompletionStage<ZLinkStoreWriteResult> applied = delegate.write(request, cancellation);
            if (!targetDelete || !intercepted.compareAndSet(false, true)) {
                return applied;
            }
            return applied.thenCompose(
                    result -> {
                        deleteApplied.complete(null);
                        return releaseDelete.thenApply(ignored -> result);
                    });
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }

    public static final class EchoInstanceSpot implements ZLinkInstanceSpot {
        static final AtomicInteger initializations = new AtomicInteger();
        static final List<Long> generations = new CopyOnWriteArrayList<>();
        static final AtomicInteger sends = new AtomicInteger();
        static final AtomicReference<Boolean> closes = new AtomicReference<>();
        static volatile CompletableFuture<Boolean> closeCompleted = new CompletableFuture<>();
        static final AtomicInteger closingCalls = new AtomicInteger();
        static final AtomicReference<ZLinkSpotCloseReason> closeReason = new AtomicReference<>();
        static volatile CompletableFuture<Void> idleEvicted = new CompletableFuture<>();
        static volatile CompletableFuture<Void> closingEntered = new CompletableFuture<>();
        static volatile CompletableFuture<Void> closingRelease =
                CompletableFuture.completedFuture(null);
        static volatile RuntimeException closingFailure;
        private final ZLinkInstanceSpotContext context;

        public EchoInstanceSpot(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkInstanceSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addPacket(EchoHandler.class);
            context.handlers().addPacket(EchoPacketHandler.class);
            context.handlers().addPacket(CloseHandler.class);
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            initializations.incrementAndGet();
            generations.add(context.objectGeneration());
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
            closingCalls.incrementAndGet();
            closingEntered.complete(null);
            closeReason.set(closing.reason());
            if (closing.reason() == ZLinkSpotCloseReason.IDLE_EVICTED) {
                idleEvicted.complete(null);
            }
            return closingFailure == null
                    ? closingRelease
                    : CompletableFuture.failedFuture(closingFailure);
        }
    }

    public static final class EchoHandler
            implements ZLinkSpotRequestHandler<EchoInstanceSpot, String, String> {
        @Override
        public CompletionStage<String> handle(EchoInstanceSpot spot, String request) {
            return CompletableFuture.completedFuture("echo:" + request);
        }
    }

    public static final class EchoPacketHandler
            implements ZLinkSpotPacketHandler<EchoInstanceSpot, Warmup> {
        @Override
        public CompletionStage<Void> handle(EchoInstanceSpot spot, Warmup request) {
            EchoInstanceSpot.sends.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class CloseHandler
            implements ZLinkSpotPacketHandler<EchoInstanceSpot, CloseInstance> {
        @Override
        public CompletionStage<Void> handle(EchoInstanceSpot spot, CloseInstance request) {
            spot.context()
                    .close()
                    .whenComplete(
                            (closed, error) -> {
                                if (error != null) {
                                    EchoInstanceSpot.closeCompleted.completeExceptionally(error);
                                } else {
                                    EchoInstanceSpot.closes.set(closed);
                                    EchoInstanceSpot.closeCompleted.complete(closed);
                                }
                            });
            return CompletableFuture.completedFuture(null);
        }
    }
}
