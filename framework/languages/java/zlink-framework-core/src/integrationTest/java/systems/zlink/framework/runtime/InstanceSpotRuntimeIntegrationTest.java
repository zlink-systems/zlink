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
        EchoInstanceSpot.warmups.set(0);
        EchoInstanceSpot.closes.set(null);
        EchoInstanceSpot.closeCompleted = new CompletableFuture<>();
        SourceEntrySpot.reset();
        SourceEntrySpot.afterCloseStart = new CompletableFuture<>();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        var store = new ZLinkInMemoryLocationStore();

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.addRelocationStore(new InMemoryRelocationStore());
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
            awaitPeersReady(
                    source, target, "instance-source-" + suffix, "instance-target-" + suffix);
            SourceEntrySpot.start.complete(null);
            assertTrue(EchoInstanceSpot.closeCompleted.get(5, TimeUnit.SECONDS));
            SourceEntrySpot.afterCloseStart.complete(null);
            String reply = SourceEntrySpot.reply.get();

            assertEquals("echo:hello|echo:again|echo:after-close", reply);
            assertEquals(2, EchoInstanceSpot.initializations.get());
            assertEquals(1, EchoInstanceSpot.warmups.get());
            assertTrue(EchoInstanceSpot.closes.get());
        }
    }

    @Test
    void authorityMissingIsNotPublishedBeforeLocalInstanceRetires() throws Exception {
        Zlink.version();
        EchoInstanceSpot.initializations.set(0);
        EchoInstanceSpot.generations.clear();
        EchoInstanceSpot.warmups.set(0);
        EchoInstanceSpot.closes.set(null);
        EchoInstanceSpot.closingCalls.set(0);
        SourceEntrySpot.reset();
        SourceEntrySpot.afterCloseStart = new CompletableFuture<>();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        String spotId = "close-order-" + suffix;
        var store = new GatedDeleteStore(new ZLinkInMemoryLocationStore(), spotId);

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.addRelocationStore(new InMemoryRelocationStore());
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
                awaitPeersReady(
                        source,
                        target,
                        "close-order-source-" + suffix,
                        "close-order-target-" + suffix);
                SourceEntrySpot.start.complete(null);
                store.deleteApplied.get(5, TimeUnit.SECONDS);

                // Spot address messaging §7 step 2-3: OnClosing and local cleanup finish before
                // the authority release. The activation record keeps the Spot lane for messages
                // placed behind Close until step 3 ends, so the count is not read here.
                assertEquals(1, EchoInstanceSpot.closingCalls.get());
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
        verifyCloseFailure(CloseScenario.RELEASE);
    }

    @Test
    void failedClosingCommitKeepsAuthorityAndAdmission() throws Exception {
        verifyCloseFailure(CloseScenario.COMMIT);
    }

    @Test
    void failedOnClosingStillReleasesAuthorityOnce() throws Exception {
        verifyCloseFailure(CloseScenario.CALLBACK);
    }

    @Test
    void failedInstanceResourceReleaseResumesAtRelease() throws Exception {
        verifyCloseFailure(CloseScenario.RESOURCE);
    }

    @Test
    void requestWithoutIntentDuringAuthorityReleaseDoesNotRun() throws Exception {
        verifyCloseFailure(CloseScenario.WINDOW);
    }

    @Test
    void requestWithoutIntentAfterClosingCommitDoesNotRun() throws Exception {
        verifyCloseFailure(CloseScenario.CAS_WINDOW);
    }

    @Test
    void directMissingWithoutIntentDoesNotRun() throws Exception {
        verifyCloseFailure(CloseScenario.MISSING);
    }

    @Test
    void directCreatingWithoutIntentIsNotFound() throws Exception {
        verifyCloseFailure(CloseScenario.CREATING);
    }

    @Test
    void intentAfterCloseReleaseDecisionIsRefusedBeforeAdmission() throws Exception {
        verifyCloseFailure(CloseScenario.CREATING_SEALED);
    }

    @Test
    void intentAfterCloseCompletionIsRefusedBeforeAdmission() throws Exception {
        verifyCloseFailure(CloseScenario.CREATING_RELEASED);
    }

    @Test
    void lateIntentUsingPreviousActivationRunsOnSuccessor() throws Exception {
        verifyCloseFailure(CloseScenario.CREATING_REPLACED);
    }

    @Test
    void lateRequestWithoutIntentUsingPreviousActivationRunsOnSuccessor() throws Exception {
        verifyCloseFailure(CloseScenario.CREATING_REPLACED, false);
    }

    @Test
    void instanceIntentAdmissionReadBeforeCloseCompletionRetainsPayload() throws Exception {
        verifyCloseFailure(CloseScenario.CREATING_ADMISSION_BEFORE_CLOSE);
    }

    @Test
    void instanceIntentAdmissionReadCrossingCloseCompletionRetainsPayload() throws Exception {
        verifyCloseFailure(CloseScenario.CREATING_ADMISSION_AFTER_CLOSE);
    }

    @Test
    void instanceIntentAuthorityReadCrossingCloseCompletionRetainsPayload() throws Exception {
        verifyCloseFailure(CloseScenario.CREATING_AUTHORITY_AFTER_CLOSE);
    }

    @Test
    void creatingIntentCrossingClosePendingCutRetainsPayloadUntilSuccessorCompletes()
            throws Exception {
        verifyCloseFailure(CloseScenario.CREATING_RETAINED);
    }

    @Test
    void secondAuthorityReleaseFailureResumesOnSameOwnerReentry() throws Exception {
        verifyCloseFailure(CloseScenario.DOUBLE_RELEASE);
    }

    @Test
    void appliedClosingCommitFailureContinuesFromCommittedAuthority() throws Exception {
        verifyCloseFailure(CloseScenario.COMMIT_APPLIED_FAILURE);
    }

    @Test
    void inconclusiveClosingCommitResumesOnSameOwnerReentry() throws Exception {
        verifyCloseFailure(CloseScenario.COMMIT_APPLIED_READ_FAILURE);
    }

    private enum CloseScenario {
        RELEASE,
        COMMIT,
        CALLBACK,
        RESOURCE,
        WINDOW,
        CAS_WINDOW,
        MISSING,
        CREATING,
        CREATING_SEALED,
        CREATING_RELEASED,
        CREATING_REPLACED,
        CREATING_RETAINED,
        CREATING_ADMISSION_BEFORE_CLOSE,
        CREATING_ADMISSION_AFTER_CLOSE,
        CREATING_AUTHORITY_AFTER_CLOSE,
        DOUBLE_RELEASE,
        COMMIT_APPLIED_FAILURE,
        COMMIT_APPLIED_READ_FAILURE
    }

    private void verifyCloseFailure(CloseScenario failureStage) throws Exception {
        verifyCloseFailure(failureStage, true);
    }

    private void verifyCloseFailure(CloseScenario failureStage, boolean lateInstanceIntent)
            throws Exception {
        boolean creating =
                failureStage == CloseScenario.CREATING
                        || failureStage == CloseScenario.CREATING_RETAINED
                        || failureStage == CloseScenario.CREATING_ADMISSION_BEFORE_CLOSE
                        || failureStage == CloseScenario.CREATING_ADMISSION_AFTER_CLOSE
                        || failureStage == CloseScenario.CREATING_AUTHORITY_AFTER_CLOSE
                        || failureStage == CloseScenario.CREATING_SEALED
                        || failureStage == CloseScenario.CREATING_RELEASED
                        || failureStage == CloseScenario.CREATING_REPLACED;
        boolean admissionBoundary =
                failureStage == CloseScenario.CREATING_ADMISSION_BEFORE_CLOSE
                        || failureStage == CloseScenario.CREATING_ADMISSION_AFTER_CLOSE
                        || failureStage == CloseScenario.CREATING_AUTHORITY_AFTER_CLOSE;
        boolean retainedBoundary =
                failureStage == CloseScenario.CREATING_RETAINED || admissionBoundary;
        boolean replacedBoundary = failureStage == CloseScenario.CREATING_REPLACED;
        boolean releasedBoundary = failureStage == CloseScenario.CREATING_RELEASED;
        boolean sealedBoundary = failureStage == CloseScenario.CREATING_SEALED || releasedBoundary;
        boolean failRelease =
                failureStage == CloseScenario.RELEASE
                        || failureStage == CloseScenario.DOUBLE_RELEASE;
        boolean failCommit = failureStage == CloseScenario.COMMIT;
        Zlink.version();
        EchoInstanceSpot.closes.set(null);
        EchoInstanceSpot.closingCalls.set(0);
        EchoInstanceSpot.generations.clear();
        EchoInstanceSpot.closingEntered = new CompletableFuture<>();
        EchoInstanceSpot.closeCompleted = new CompletableFuture<>();
        EchoInstanceSpot.closingRelease = CompletableFuture.completedFuture(null);
        EchoInstanceSpot.closingFailure =
                failureStage == CloseScenario.CALLBACK
                        ? new IllegalStateException("injected OnClosing failure")
                        : null;
        SourceEntrySpot.reset();
        SourceEntrySpot.closeStart = new CompletableFuture<>();
        if (creating) SourceEntrySpot.afterCloseStart = new CompletableFuture<>();
        if (failCommit
                || failureStage == CloseScenario.WINDOW
                || failureStage == CloseScenario.CAS_WINDOW
                || failureStage == CloseScenario.MISSING
                || creating) {
            SourceEntrySpot.probeStart = new CompletableFuture<>();
            SourceEntrySpot.probeFailure = new CompletableFuture<>();
            SourceEntrySpot.probeDirect = true;
        }
        if (failureStage == CloseScenario.MISSING) {
            SourceEntrySpot.closeStart.complete(null);
            SourceEntrySpot.afterCloseStart = new CompletableFuture<>();
        }
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        String spotId = "release-failure-" + suffix;
        var store =
                new GatedDeleteStore(
                        new ZLinkInMemoryLocationStore(),
                        spotId,
                        failureStage == CloseScenario.DOUBLE_RELEASE ? 2 : failRelease ? 1 : 0,
                        failureStage == CloseScenario.WINDOW || sealedBoundary,
                        creating);
        if (failureStage != CloseScenario.WINDOW && !sealedBoundary) {
            store.releaseDelete.complete(null);
        }
        if (retainedBoundary || sealedBoundary || replacedBoundary) {
            EchoInstanceSpot.retainedCalls.clear();
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
        if (failureStage == CloseScenario.CALLBACK) {
            diagnosticLogger.addHandler(diagnosticHandler);
        }
        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.addRelocationStore(new InMemoryRelocationStore());
        targetOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        if (failureStage == CloseScenario.CALLBACK || sealedBoundary || retainedBoundary) {
            targetOptions
                    .configureDispatch()
                    .messageFlow(
                            systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        }
        targetOptions
                .addRouteMesh("game")
                .listen("tcp://127.0.0.1:0")
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
        if (retainedBoundary) {
            sourceOptions
                    .configureDispatch()
                    .messageFlow(
                            systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        }
        var sourceNode = sourceOptions.addRouteMesh("game");
        sourceNode
                .listen("tcp://127.0.0.1:0")
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
            awaitPeersReady(
                    source,
                    target,
                    "release-failure-source-" + suffix,
                    "release-failure-target-" + suffix);
            SourceEntrySpot.start.complete(null);
            try {
                if (creating || failureStage == CloseScenario.MISSING) {
                    if (creating) {
                        store.readyPending.get(5, TimeUnit.SECONDS);
                    } else {
                        store.deleteApplied.get(5, TimeUnit.SECONDS);
                        // The probe starts after Close has ended, so the target no longer holds
                        // the Spot and the source sends on the Ready route it read before Close.
                        assertTrue(EchoInstanceSpot.closeCompleted.get(5, TimeUnit.SECONDS));
                        assertEquals(0, target.activeSpotCount("game"));
                    }
                    SourceEntrySpot.probeStart.complete(null);
                    Throwable probe = SourceEntrySpot.probeFailure.get(5, TimeUnit.SECONDS);
                    // creating: spot address messaging §9, a direct call without Instance intent
                    // to a Creating authority is NotFound. missing: the released Ready route is
                    // refused by its owner fence (spot-close-v1.json
                    // stale-released-ready-owner-fence-request).
                    assertProbeKind(
                            creating
                                    ? ZLinkFrameworkErrorKind.NOT_FOUND
                                    : ZLinkFrameworkErrorKind.UNAVAILABLE,
                            probe);
                    store.releaseReady.complete(null);
                    if (replacedBoundary) {
                        SourceEntrySpot.beforeClose.get(5, TimeUnit.SECONDS);
                        try (var seed = new RetainedAdmissionBoundary(target, spotId);
                                var late =
                                        new RetainedAdmissionBoundary(
                                                target, spotId, lateInstanceIntent)) {
                            seed.resumeClose.complete(null);
                            late.start.complete(null);
                            late.beforeAdmission.get(5, TimeUnit.SECONDS);
                            SourceEntrySpot.closeStart.complete(null);
                            late.afterPendingCut.get(5, TimeUnit.SECONDS);
                            seed.start.complete(null);
                            seed.beforeAdmission.get(5, TimeUnit.SECONDS);
                            seed.resumeAdmission.complete(null);
                            seed.admitted.get(5, TimeUnit.SECONDS);
                            late.resumeClose.complete(null);
                            assertTrue(EchoInstanceSpot.closeCompleted.get(5, TimeUnit.SECONDS));
                            assertEquals("echo:retained", seed.reply.get(5, TimeUnit.SECONDS));
                            late.resumeAdmission.complete(null);
                            late.admitted.get(5, TimeUnit.SECONDS);
                            assertEquals("echo:retained", late.reply.get(5, TimeUnit.SECONDS));
                            assertEquals(
                                    List.of(
                                            EchoInstanceSpot.generations.getLast(),
                                            EchoInstanceSpot.generations.getLast()),
                                    EchoInstanceSpot.retainedCalls);
                            assertNotEquals(
                                    EchoInstanceSpot.generations.getFirst(),
                                    EchoInstanceSpot.generations.getLast());
                            seed.released.get(5, TimeUnit.SECONDS);
                            late.released.get(5, TimeUnit.SECONDS);
                            assertEquals(1, seed.releases.get());
                            assertEquals(1, late.releases.get());
                            SourceEntrySpot.afterCloseStart.complete(null);
                            assertEquals(
                                    "echo:hello|echo:again|echo:after-close",
                                    SourceEntrySpot.reply.get(5, TimeUnit.SECONDS));
                        }
                        return;
                    }
                    if (retainedBoundary || sealedBoundary) {
                        if (retainedBoundary && !admissionBoundary)
                            SourceEntrySpot.afterCloseStart.complete(null);
                        SourceEntrySpot.beforeClose.get(5, TimeUnit.SECONDS);
                        try (var boundary = new RetainedAdmissionBoundary(target, spotId)) {
                            if (!sealedBoundary) {
                                boundary.start.complete(null);
                                boundary.beforeAdmission.get(5, TimeUnit.SECONDS);
                            }
                            SourceEntrySpot.closeStart.complete(null);
                            boundary.afterPendingCut.get(5, TimeUnit.SECONDS);
                            assertEquals(0, boundary.releases.get());
                            if (sealedBoundary) {
                                boundary.resumeClose.complete(null);
                                store.deleteAttempted.get(5, TimeUnit.SECONDS);
                                boundary.start.complete(null);
                                boundary.beforeAdmission.get(5, TimeUnit.SECONDS);
                                if (releasedBoundary) {
                                    store.releaseDelete.complete(null);
                                    assertTrue(
                                            EchoInstanceSpot.closeCompleted.get(
                                                    5, TimeUnit.SECONDS));
                                }
                                boundary.resumeAdmission.complete(null);
                                // §7 step 3 ends admission together with the release decision.
                                // This message reaches that old owner only after the decision.
                                assertThrows(
                                        java.util.concurrent.ExecutionException.class,
                                        () -> boundary.admitted.get(5, TimeUnit.SECONDS));
                                var refused =
                                        assertThrows(
                                                        java.util.concurrent.ExecutionException
                                                                .class,
                                                        () ->
                                                                boundary.reply.get(
                                                                        5, TimeUnit.SECONDS))
                                                .getCause();
                                assertProbeKind(ZLinkFrameworkErrorKind.UNAVAILABLE, refused);
                                assertEquals(
                                        systems.zlink.framework.runtime.protocol
                                                .ServiceWireConstants.FRAMEWORK_ERROR_SPOT_MOVING,
                                        systems.zlink.framework.runtime.internal.backend
                                                .ZLinkRequestFailureMapping.causeCode(refused));
                                boundary.released.get(5, TimeUnit.SECONDS);
                                assertEquals(1, boundary.releases.get());
                                assertTrue(EchoInstanceSpot.retainedCalls.isEmpty());
                                store.releaseDelete.complete(null);
                                assertTrue(
                                        EchoInstanceSpot.closeCompleted.get(5, TimeUnit.SECONDS));
                                SourceEntrySpot.afterCloseStart.complete(null);
                                assertEquals(
                                        "echo:hello|echo:again|echo:after-close",
                                        SourceEntrySpot.reply.get(5, TimeUnit.SECONDS));
                                return;
                            }
                            boundary.resumeAdmission.complete(null);
                            boundary.admitted.get(5, TimeUnit.SECONDS);
                            assertEquals(0, boundary.releases.get());
                            if (admissionBoundary) {
                                try (var readBoundary =
                                        new InstanceAdmissionReadBoundary(
                                                target,
                                                spotId,
                                                failureStage
                                                        == CloseScenario
                                                                .CREATING_AUTHORITY_AFTER_CLOSE)) {
                                    SourceEntrySpot.afterCloseStart.complete(null);
                                    readBoundary.reached.get(5, TimeUnit.SECONDS);
                                    if (failureStage
                                            == CloseScenario.CREATING_ADMISSION_BEFORE_CLOSE) {
                                        readBoundary.resume.complete(null);
                                        readBoundary.received.get(5, TimeUnit.SECONDS);
                                    }
                                    readBoundary.logger.info(
                                            "test_lifecycle=resume_close spot=" + spotId);
                                    boundary.resumeClose.complete(null);
                                    assertEquals(
                                            "echo:retained",
                                            boundary.reply.get(5, TimeUnit.SECONDS));
                                    assertTrue(
                                            EchoInstanceSpot.closeCompleted.get(
                                                    5, TimeUnit.SECONDS));
                                    assertEquals(
                                            List.of(EchoInstanceSpot.generations.getLast()),
                                            EchoInstanceSpot.retainedCalls);
                                    assertNotEquals(
                                            EchoInstanceSpot.generations.getFirst(),
                                            EchoInstanceSpot.retainedCalls.getFirst());
                                    readBoundary.logger.info(
                                            "test_lifecycle=close_completed spot="
                                                    + spotId
                                                    + " generations="
                                                    + EchoInstanceSpot.generations);
                                    boundary.released.get(5, TimeUnit.SECONDS);
                                    assertEquals(1, boundary.releases.get());
                                    readBoundary.resume.complete(null);
                                    assertEquals(
                                            "echo:hello|echo:again|echo:after-close",
                                            SourceEntrySpot.reply.get(5, TimeUnit.SECONDS));
                                }
                                return;
                            }
                            boundary.resumeClose.complete(null);
                            assertEquals("echo:retained", boundary.reply.get(5, TimeUnit.SECONDS));
                            assertEquals(
                                    "echo:hello|echo:again|echo:after-close",
                                    SourceEntrySpot.reply.get(5, TimeUnit.SECONDS));
                            assertEquals(
                                    List.of(EchoInstanceSpot.generations.getLast()),
                                    EchoInstanceSpot.retainedCalls);
                            assertNotEquals(
                                    EchoInstanceSpot.generations.getFirst(),
                                    EchoInstanceSpot.retainedCalls.getFirst());
                            boundary.released.get(5, TimeUnit.SECONDS);
                            assertEquals(1, boundary.releases.get());
                        }
                        return;
                    }
                    if (creating) {
                        SourceEntrySpot.closeStart.complete(null);
                        // A send terminal completes the CloseInstance handler, which only
                        // requests Close. The next cold call starts after Close itself ends.
                        assertTrue(EchoInstanceSpot.closeCompleted.get(5, TimeUnit.SECONDS));
                        assertEquals(0, target.activeSpotCount("game"));
                    }
                    SourceEntrySpot.afterCloseStart.complete(null);
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
                } else if (failureStage == CloseScenario.COMMIT_APPLIED_FAILURE
                        || failureStage == CloseScenario.COMMIT_APPLIED_READ_FAILURE) {
                    store.failNextAuthorityPutAfterApply.set(true);
                    store.failReadAfterAppliedPut.set(
                            failureStage == CloseScenario.COMMIT_APPLIED_READ_FAILURE);
                } else if (failureStage == CloseScenario.CAS_WINDOW) {
                    store.holdNextAuthorityPutAfterApply.set(true);
                }
                var field = ZLinkFrameworkRuntime.class.getDeclaredField("spots");
                field.setAccessible(true);
                var spots = field.get(target);
                AtomicInteger completedBackendOperation =
                        failureStage == CloseScenario.RESOURCE
                                ? installBackendFailure(spots, spotId, "closeInstanceSpot")
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
                if (failureStage == CloseScenario.DOUBLE_RELEASE) {
                    store.deleteFailed.get(5, TimeUnit.SECONDS);
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> closing.toCompletableFuture().get(5, TimeUnit.SECONDS));
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
                    // The activation record holds the Spot lane until the release succeeds.
                    assertEquals(0, target.activeSpotCount("game"));
                    assertEquals(1, EchoInstanceSpot.closingCalls.get());
                    return;
                }
                if (failureStage == CloseScenario.COMMIT_APPLIED_READ_FAILURE) {
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
                if (failureStage == CloseScenario.CAS_WINDOW) {
                    store.closingApplied.get(5, TimeUnit.SECONDS);
                    SourceEntrySpot.probeStart.complete(null);
                    // Spot address messaging §7 step 1: the probe reaches the owner while Close
                    // runs and waits behind it. §9 Closing row: without Instance intent it ends
                    // with NotFound once Close ends.
                    awaitPendingMessage(spots, spotId);
                    store.releaseClosing.complete(null);
                    assertProbeKind(
                            ZLinkFrameworkErrorKind.NOT_FOUND,
                            SourceEntrySpot.probeFailure.get(5, TimeUnit.SECONDS));
                    assertTrue(closing.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    assertEquals(1, EchoInstanceSpot.closingCalls.get());
                    return;
                }
                if (failureStage == CloseScenario.WINDOW) {
                    store.deleteAttempted.get(5, TimeUnit.SECONDS);
                    SourceEntrySpot.probeStart.complete(null);
                    // The authority is Closing until the held Delete applies. §9 Closing row: a
                    // request without Instance intent ends with NotFound before the release.
                    assertProbeKind(
                            ZLinkFrameworkErrorKind.NOT_FOUND,
                            SourceEntrySpot.probeFailure.get(5, TimeUnit.SECONDS));
                    store.releaseDelete.complete(null);
                    assertTrue(closing.toCompletableFuture().get(5, TimeUnit.SECONDS));
                    assertEquals(0, target.activeSpotCount("game"));
                    assertEquals(1, EchoInstanceSpot.closingCalls.get());
                    return;
                }
                if (failRelease) {
                    store.deleteFailed.get(5, TimeUnit.SECONDS);
                } else if (failCommit) {
                    store.putFailed.get(5, TimeUnit.SECONDS);
                } else if (failureStage == CloseScenario.COMMIT_APPLIED_FAILURE) {
                    store.putAppliedThenFailed.get(5, TimeUnit.SECONDS);
                }
                if (failureStage == CloseScenario.CALLBACK) {
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
                    if (failureStage == CloseScenario.CALLBACK) {
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

    /** Holds target authority reads or their completion while Close replaces the incarnation. */
    private static final class InstanceAdmissionReadBoundary implements AutoCloseable {
        // SourceEntrySpot sends Warmup, hello and again before its after-close request.
        private static final String AFTER_CLOSE_CORRELATION = "corr=4";
        final CompletableFuture<Void> reached = new CompletableFuture<>();
        final CompletableFuture<Void> resume = new CompletableFuture<>();
        final CompletableFuture<Void> received = new CompletableFuture<>();
        final String spotId;
        final Object spotNode;
        final java.lang.reflect.Field resolverField;
        final systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode
                        .SpotAdmissionResolver
                original;
        final java.util.logging.Logger logger =
                java.util.logging.Logger.getLogger(
                        systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer.class
                                .getName());
        final java.util.logging.Handler listener =
                new java.util.logging.Handler() {
                    @Override
                    public void publish(java.util.logging.LogRecord record) {
                        if (record.getMessage().contains("phase=received")
                                && record.getMessage().contains(AFTER_CLOSE_CORRELATION)
                                && record.getMessage().contains("spot=" + spotId + " "))
                            received.complete(null);
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };

        InstanceAdmissionReadBoundary(
                ZLinkFrameworkRuntime runtime, String targetSpotId, boolean holdBeforeRead)
                throws Exception {
            this.spotId = targetSpotId;
            var spotsField = ZLinkFrameworkRuntime.class.getDeclaredField("spots");
            spotsField.setAccessible(true);
            Object spots = spotsField.get(runtime);
            var meshesField = spots.getClass().getDeclaredField("routeMeshNodesByName");
            meshesField.setAccessible(true);
            var mesh =
                    (systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode)
                            ((java.util.Map<?, ?>) meshesField.get(spots)).get("game");
            spotNode = mesh.spotNode();
            resolverField = spotNode.getClass().getDeclaredField("spotAdmissionResolver");
            resolverField.setAccessible(true);
            original =
                    (systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode
                                    .SpotAdmissionResolver)
                            resolverField.get(spotNode);
            logger.addHandler(listener);
            resolverField.set(
                    spotNode,
                    (systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode
                                    .SpotAdmissionResolver)
                            (spotId, ownerGeneration, intent, present, route) -> {
                                if (holdBeforeRead) {
                                    logger.info(
                                            "test_lifecycle=before_authority_read owner_generation="
                                                    + ownerGeneration);
                                    reached.complete(null);
                                    return resume.thenCompose(
                                            ignored ->
                                                    original.resolve(
                                                            spotId,
                                                            ownerGeneration,
                                                            intent,
                                                            present,
                                                            route));
                                }
                                return original.resolve(
                                                spotId, ownerGeneration, intent, present, route)
                                        .thenCompose(
                                                rejection -> {
                                                    if (rejection != null)
                                                        return CompletableFuture.completedFuture(
                                                                rejection);
                                                    logger.info(
                                                            "test_lifecycle=admission_read owner_generation="
                                                                    + ownerGeneration);
                                                    reached.complete(null);
                                                    return resume.thenApply(ignored -> rejection);
                                                });
                            });
        }

        @Override
        public void close() throws IllegalAccessException {
            resume.complete(null);
            resolverField.set(spotNode, original);
            logger.removeHandler(listener);
        }
    }

    /** Controls the existing receive/admission and backend-close execution boundaries. */
    private static final class RetainedAdmissionBoundary implements AutoCloseable {
        final CompletableFuture<Void> start = new CompletableFuture<>();
        final CompletableFuture<String> reply = new CompletableFuture<>();
        final CompletableFuture<Void> beforeAdmission = new CompletableFuture<>();
        final CompletableFuture<Void> resumeAdmission = new CompletableFuture<>();
        final CompletableFuture<Void> afterPendingCut = new CompletableFuture<>();
        final CompletableFuture<Void> resumeClose = new CompletableFuture<>();
        final CompletableFuture<Void> admitted = new CompletableFuture<>();
        final CompletableFuture<Void> released = new CompletableFuture<>();
        final AtomicInteger releases = new AtomicInteger();
        final java.util.concurrent.ExecutorService executor =
                java.util.concurrent.Executors.newVirtualThreadPerTaskExecutor();

        RetainedAdmissionBoundary(ZLinkFrameworkRuntime runtime, String spotId) throws Exception {
            this(runtime, spotId, true);
        }

        RetainedAdmissionBoundary(
                ZLinkFrameworkRuntime runtime, String spotId, boolean instanceIntent)
                throws Exception {
            var spotsField = ZLinkFrameworkRuntime.class.getDeclaredField("spots");
            spotsField.setAccessible(true);
            Object spots = spotsField.get(runtime);
            var activations = spots.getClass().getDeclaredField("instanceSpotActivations");
            activations.setAccessible(true);
            Object activation = ((java.util.Map<?, ?>) activations.get(spots)).get(spotId);
            var base = activation.getClass().getSuperclass();
            var backendField = base.getDeclaredField("backendSpot");
            backendField.setAccessible(true);
            Object backend = backendField.get(activation);
            var route =
                    new systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                            .InstanceColdActivation(
                            RoutingId.from("retained-fixture"),
                            1,
                            spotId,
                            "game",
                            "EchoInstance",
                            "descriptor",
                            java.time.Instant.now().plusSeconds(5).toEpochMilli());
            var originals =
                    List.of(
                            systems.zlink.contracts.messaging.Message.from(
                                    systems.zlink.framework.runtime.messaging.ZLinkPacketNames
                                            .resolve(String.class)),
                            systems.zlink.contracts.messaging.Message.from("\"retained\""));
            var parts =
                    new java.util.AbstractList<systems.zlink.contracts.messaging.Message>() {
                        @Override
                        public int size() {
                            return originals.size();
                        }

                        @Override
                        public systems.zlink.contracts.messaging.Message get(int index) {
                            if (!beforeAdmission.isDone()) {
                                beforeAdmission.complete(null);
                                resumeAdmission.orTimeout(5, TimeUnit.SECONDS).join();
                            }
                            return originals.get(index);
                        }
                    };
            var received =
                    new systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived(
                            systems.zlink.framework.runtime.internal.backend
                                    .ZLinkBackendRequestResult.OK,
                            java.util.Optional.empty(),
                            java.util.Optional.of(spotId),
                            java.util.Optional.of(1L),
                            new byte[0],
                            new byte[0],
                            parts,
                            replyParts -> {
                                try {
                                    reply.complete(
                                            new com.fasterxml.jackson.databind.ObjectMapper()
                                                    .readValue(
                                                            replyParts.getLast().toByteArray(),
                                                            String.class));
                                } catch (java.io.IOException failure) {
                                    reply.completeExceptionally(failure);
                                } finally {
                                    replyParts.forEach(
                                            systems.zlink.contracts.messaging.Message::close);
                                }
                            },
                            () -> {
                                releases.incrementAndGet();
                                released.complete(null);
                            },
                            "application/json");
            if (instanceIntent)
                received.retainActivationMessage(
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceM6BWireCodec.InstanceSpotMessage(
                                0,
                                route,
                                true,
                                route.targetNodeGeneration(),
                                route.targetNodeRid(),
                                "source",
                                true,
                                1,
                                1,
                                null));
            var admit =
                    activation
                            .getClass()
                            .getDeclaredMethod(
                                    "admitRoute",
                                    systems.zlink.framework.runtime.internal.backend
                                            .ZLinkBackendReceived.class,
                                    CompletableFuture.class);
            admit.setAccessible(true);
            executor.submit(
                    () -> {
                        try {
                            start.get(5, TimeUnit.SECONDS);
                            var completion =
                                    (CompletionStage<?>)
                                            admit.invoke(activation, received, admitted);
                            completion.whenComplete(
                                    (value, failure) -> {
                                        if (failure != null) reply.completeExceptionally(failure);
                                    });
                        } catch (ReflectiveOperationException
                                | java.util.concurrent.ExecutionException
                                | java.util.concurrent.TimeoutException failure) {
                            admitted.completeExceptionally(failure);
                        } catch (InterruptedException failure) {
                            Thread.currentThread().interrupt();
                            admitted.completeExceptionally(failure);
                        }
                    });
            Object proxy =
                    java.lang.reflect.Proxy.newProxyInstance(
                            backend.getClass().getClassLoader(),
                            new Class<?>[] {
                                systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot
                                        .class
                            },
                            (ignored, method, args) -> {
                                if (method.getName().equals("close")) {
                                    afterPendingCut.complete(null);
                                    resumeClose.get(5, TimeUnit.SECONDS);
                                }
                                try {
                                    return method.invoke(backend, args);
                                } catch (java.lang.reflect.InvocationTargetException failure) {
                                    throw failure.getCause();
                                }
                            });
            backendField.set(activation, proxy);
            var contextField = base.getDeclaredField("context");
            contextField.setAccessible(true);
            Object context = contextField.get(activation);
            var contextBackend = context.getClass().getDeclaredField("backendSpot");
            contextBackend.setAccessible(true);
            contextBackend.set(context, proxy);
        }

        @Override
        public void close() {
            resumeAdmission.complete(null);
            resumeClose.complete(null);
            executor.close();
        }
    }

    private static void assertProbeKind(ZLinkFrameworkErrorKind expected, Throwable failure) {
        assertTrue(failure instanceof ZLinkFrameworkException, String.valueOf(failure));
        assertEquals(expected, ((ZLinkFrameworkException) failure).kind());
    }

    /**
     * Waits until each RouteMesh node sees the other as a ready peer. The RouteMesh send path does
     * not wait for a peer (00-foundation/06-framework-api.ko.md channel selection result), so the
     * scenario starts only after both connections are ready.
     */
    private static void awaitPeersReady(
            ZLinkFrameworkRuntime source,
            ZLinkFrameworkRuntime target,
            String sourceRid,
            String targetRid)
            throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!peerReady(source, targetRid) || !peerReady(target, sourceRid)) {
            if (System.nanoTime() > deadline) {
                throw new java.util.concurrent.TimeoutException(
                        "RouteMesh peers never became ready");
            }
            Thread.sleep(2);
        }
    }

    private static boolean peerReady(ZLinkFrameworkRuntime runtime, String peerRid) {
        RoutingId rid = RoutingId.from(peerRid);
        return runtime.routeMeshRuntime().snapshot("game").peers().stream()
                .anyMatch(
                        peer ->
                                peer.nodeRid().equals(rid)
                                        && peer.state()
                                                == systems.zlink.framework.monitoring.ZLinkPeerState
                                                        .READY);
    }

    /** Waits until the target's route scan has dropped the Closing Instance authority. */
    private static void awaitClosingRouteDropped(Object spots, String spotId) throws Exception {
        var meshes = spots.getClass().getDeclaredField("routeMeshNodesByName");
        meshes.setAccessible(true);
        var mesh =
                (systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode)
                        ((java.util.Map<?, ?>) meshes.get(spots)).get("game");
        Object spotNode = mesh.spotNode();
        var authorities = spotNode.getClass().getDeclaredField("instanceAuthorities");
        authorities.setAccessible(true);
        var registered = (java.util.Map<?, ?>) authorities.get(spotNode);
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (registered.containsKey(spotId)) {
            if (System.nanoTime() > deadline) {
                throw new java.util.concurrent.TimeoutException(
                        "route scan kept the Closing Instance authority");
            }
            Thread.sleep(2);
        }
    }

    /** Waits until the Instance Spot owner queue holds an accepted message behind Close. */
    private static void awaitPendingMessage(Object spots, String spotId) throws Exception {
        var activations = spots.getClass().getDeclaredField("instanceSpotActivations");
        activations.setAccessible(true);
        Object activation = ((java.util.Map<?, ?>) activations.get(spots)).get(spotId);
        var contextField = activation.getClass().getSuperclass().getDeclaredField("context");
        contextField.setAccessible(true);
        Object context = contextField.get(activation);
        var ownerQueue = context.getClass().getDeclaredMethod("ownerQueue");
        ownerQueue.setAccessible(true);
        var queue =
                (systems.zlink.framework.execution.ZLinkSerialExecutionQueue)
                        ownerQueue.invoke(context);
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (queue.pendingMessages().toCompletableFuture().join().isEmpty()) {
            if (System.nanoTime() > deadline) {
                throw new java.util.concurrent.TimeoutException("probe never reached the owner");
            }
            Thread.sleep(2);
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
    void publicRequestToClosingInstanceRunsOnNextIncarnation() throws Exception {
        // Spot address messaging §7 step 1 and 3, §9 Closing row: an Instance intent request
        // that reaches the owner during Close waits behind Close and runs on the next
        // incarnation.
        verifyClosingInstanceVerdict(false, null);
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
        SourceEntrySpot.closeStart = new CompletableFuture<>();
        SourceEntrySpot.probeStart = new CompletableFuture<>();
        SourceEntrySpot.probeFailure = new CompletableFuture<>();
        SourceEntrySpot.afterCloseStart = new CompletableFuture<>();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        String spotId = "closing-verdict-" + suffix;
        var store = new ZLinkInMemoryLocationStore();

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions.addRelocationStore(new InMemoryRelocationStore());
        targetOptions.configureLocations().setPollingInterval(Duration.ofMillis(20));
        targetOptions
                .addRouteMesh("game")
                .listen("tcp://127.0.0.1:0")
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
                .listen("tcp://127.0.0.1:0")
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
                awaitPeersReady(
                        source,
                        target,
                        "closing-verdict-source-" + suffix,
                        "closing-verdict-target-" + suffix);
                SourceEntrySpot.start.complete(null);
                SourceEntrySpot.beforeClose.get(5, TimeUnit.SECONDS);
                // Establish the scan-owned Ready projection before testing its Closing removal.
                var routesField =
                        ZLinkFrameworkRuntime.class.getDeclaredField("authorityRouteRuntime");
                routesField.setAccessible(true);
                ((systems.zlink.framework.runtime.locations.ZLinkStatefulAuthorityRouteRuntime)
                                routesField.get(target))
                        .reconcile()
                        .toCompletableFuture()
                        .get(5, TimeUnit.SECONDS);
                SourceEntrySpot.closeStart.complete(null);
                EchoInstanceSpot.closingEntered.get(5, TimeUnit.SECONDS);
                if (drain) {
                    var field = ZLinkFrameworkRuntime.class.getDeclaredField("spots");
                    field.setAccessible(true);
                    ((systems.zlink.framework.runtime.spots.ZLinkSpotRuntime) field.get(target))
                            .beginDrain()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
                }
                if (expectedKind == null) {
                    // The target's background route scan drops the Closing authority before the
                    // probe arrives, so the probe's admission cannot rely on that scan.
                    var spotsField = ZLinkFrameworkRuntime.class.getDeclaredField("spots");
                    spotsField.setAccessible(true);
                    awaitClosingRouteDropped(spotsField.get(target), spotId);
                }
                SourceEntrySpot.probeStart.complete(null);
                if (expectedKind == null) {
                    // OnClosing stays held until the probe waits in the owner queue, so step 3
                    // sees the pending Instance intent message and creates the next incarnation.
                    var spotsField = ZLinkFrameworkRuntime.class.getDeclaredField("spots");
                    spotsField.setAccessible(true);
                    awaitPendingMessage(spotsField.get(target), spotId);
                    EchoInstanceSpot.closingRelease.complete(null);
                    assertEquals(
                            "echo:during-close",
                            SourceEntrySpot.probeReply.get(5, TimeUnit.SECONDS));
                } else {
                    Throwable failure = SourceEntrySpot.probeFailure.get(5, TimeUnit.SECONDS);
                    assertTrue(failure instanceof ZLinkFrameworkException);
                    assertEquals(expectedKind, ((ZLinkFrameworkException) failure).kind());
                }
            } finally {
                SourceEntrySpot.closeStart.complete(null);
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
        EchoInstanceSpot.warmups.set(0);
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
        targetOptions.addRelocationStore(new InMemoryRelocationStore());
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
            awaitPeersReady(
                    source,
                    target,
                    "instance-idle-source-" + suffix,
                    "instance-idle-target-" + suffix);
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
        static CompletableFuture<String> probeReply;
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
            probeReply = new CompletableFuture<>();
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
            // Initialization installs the scenario; the test opens start after Serving.
            // The scenario's terminal belongs to reply, independently of startup.
            start.thenCompose(
                            ignored -> {
                                CompletionStage<String> warmup =
                                        context.outbound()
                                                .requestToSpot(
                                                        request.get().spotId(),
                                                        new Warmup("warmup"))
                                                .instanceSpot("EchoInstance")
                                                .inMesh("game")
                                                .submit(String.class);
                                CompletionStage<String> first =
                                        warmup.thenCompose(
                                                warmupReply -> {
                                                    assertEquals("warmup:ready", warmupReply);
                                                    return context.outbound()
                                                            .requestToSpot(
                                                                    request.get().spotId(), "hello")
                                                            .instanceSpot("EchoInstance")
                                                            .inMesh("game")
                                                            .timeout(Duration.ofSeconds(5))
                                                            .submit(String.class);
                                                });
                                CompletionStage<String> firstAndSecond =
                                        first.thenCompose(
                                                firstValue -> {
                                                    return context.outbound()
                                                            .requestToSpot(
                                                                    request.get().spotId(), "again")
                                                            .instanceSpot()
                                                            .inMesh("game")
                                                            .timeout(Duration.ofSeconds(5))
                                                            .submit(String.class)
                                                            .thenApply(
                                                                    secondValue ->
                                                                            firstValue
                                                                                    + "|"
                                                                                    + secondValue);
                                                });
                                CompletionStage<String> beforeAfterClose =
                                        firstAndSecond.thenCompose(
                                                value -> {
                                                    beforeClose.complete(null);
                                                    return closeStart.thenCompose(
                                                            closeAllowed ->
                                                                    context.outbound()
                                                                            .sendToSpot(
                                                                                    request.get()
                                                                                            .spotId(),
                                                                                    new CloseInstance())
                                                                            .instanceSpot()
                                                                            .inMesh("game")
                                                                            .submit()
                                                                            .thenApply(
                                                                                    ignoredClose ->
                                                                                            value));
                                                });
                                if (probeStart != null) {
                                    probeStart
                                            .thenCompose(
                                                    probeStarted -> {
                                                        var outbound =
                                                                context.outbound()
                                                                        .requestToSpot(
                                                                                request.get()
                                                                                        .spotId(),
                                                                                "during-close");
                                                        return probeDirect
                                                                ? outbound.inMesh("game")
                                                                        .timeout(
                                                                                Duration.ofSeconds(
                                                                                        5))
                                                                        .submit(String.class)
                                                                : outbound.instanceSpot()
                                                                        .inMesh("game")
                                                                        .timeout(
                                                                                Duration.ofSeconds(
                                                                                        5))
                                                                        .submit(String.class);
                                                    })
                                            .whenComplete(
                                                    (value, failure) -> {
                                                        if (failure == null)
                                                            probeReply.complete(value);
                                                        probeFailure.complete(
                                                                failure == null
                                                                        ? new AssertionError(
                                                                                "Closing request was admitted")
                                                                        : failure
                                                                                        instanceof
                                                                                        CompletionException
                                                                                                wrapped
                                                                                ? wrapped.getCause()
                                                                                : failure);
                                                    });
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
                                                                                        .inMesh(
                                                                                                "game")
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
                            })
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure != null) reply.completeExceptionally(failure);
                            });
            return CompletableFuture.completedFuture(null);
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
            // The scenario starts after runtime startup and reports its terminal via reply.
            start.thenCompose(
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
                                                                            // This step waits only
                                                                            // for the
                                                                            // Missing row, not for
                                                                            // the
                                                                            // owner's Close to end.
                                                                            // A probe
                                                                            // that reaches the
                                                                            // owner while
                                                                            // Close still runs is
                                                                            // NotFound
                                                                            // (§9 Closing row); one
                                                                            // that
                                                                            // arrives after the
                                                                            // release on
                                                                            // the Ready route read
                                                                            // before
                                                                            // is refused by its
                                                                            // owner fence
                                                                            // as Unavailable (§9
                                                                            // owner fence
                                                                            // row,
                                                                            // spot-close-v1.json
                                                                            // stale-released-ready-owner-fence-request).
                                                                            assertTrue(
                                                                                    cause
                                                                                            instanceof
                                                                                            ZLinkFrameworkException,
                                                                                    String.valueOf(
                                                                                            cause));
                                                                            var kind =
                                                                                    ((ZLinkFrameworkException)
                                                                                                    cause)
                                                                                            .kind();
                                                                            assertTrue(
                                                                                    kind
                                                                                                    == ZLinkFrameworkErrorKind
                                                                                                            .NOT_FOUND
                                                                                            || kind
                                                                                                    == ZLinkFrameworkErrorKind
                                                                                                            .UNAVAILABLE,
                                                                                    kind
                                                                                            .toString());
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
                            });
            return CompletableFuture.completedFuture(null);
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
        static final List<Long> retainedCalls = new CopyOnWriteArrayList<>();
        static final AtomicInteger initializations = new AtomicInteger();
        static final List<Long> generations = new CopyOnWriteArrayList<>();
        static final AtomicInteger warmups = new AtomicInteger();
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
            context.handlers().addPacket(EchoWarmupHandler.class);
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
            if (request.equals("retained")) {
                EchoInstanceSpot.retainedCalls.add(spot.context().objectGeneration());
            }
            return CompletableFuture.completedFuture("echo:" + request);
        }
    }

    public static final class EchoWarmupHandler
            implements ZLinkSpotRequestHandler<EchoInstanceSpot, Warmup, String> {
        @Override
        public CompletionStage<String> handle(EchoInstanceSpot spot, Warmup request) {
            EchoInstanceSpot.warmups.incrementAndGet();
            return CompletableFuture.completedFuture("warmup:ready");
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
