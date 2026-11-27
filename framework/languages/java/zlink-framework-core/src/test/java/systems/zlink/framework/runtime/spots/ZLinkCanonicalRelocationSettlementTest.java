package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static systems.zlink.framework.runtime.spots.ZLinkRelocationSettlementFixture.OPEN;
import static systems.zlink.framework.runtime.spots.ZLinkRelocationSettlementFixture.STAGE_DEADLINE;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryOwner;
import systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryTestFence;
import systems.zlink.framework.runtime.host.ZLinkFrameworkTerminationOutcome;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDelete;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDeleted;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.spots.ZLinkRelocationSettlementFixture.Fixture;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

/**
 * Relocation settlement after relay readiness (spec 28 §4.4–§4.5, spec 01 §6.1/§10): the cutover
 * wait is a Warning only, a resent whole batch replaces a partial one, the source settles from the
 * Location Store, and a won source {@code Preserve} fence ends the target staging.
 */
final class ZLinkCanonicalRelocationSettlementTest {
    private static final byte[] R1 = {11, 1};
    private static final byte[] R2 = {22, 2, 2};

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "false,0,true",
        "false,1,true",
        "false,2,true",
        "true,1,true",
        "true,2,true",
        "false,2,false",
        "false,3,true"
    })
    void selectedRouteReplacementResendsTheWholeBoundaryWithoutDisconnect(
            boolean emptyBatch, int replaceAt, boolean readyAtObservation) throws Exception {
        Fixture fixture = new Fixture(true);
        fixture.silentlyDrop.set(true);
        fixture.dropCutover.set(true);
        fixture.stage();
        if (replaceAt == 3) {
            fixture.routeGeneration.set(2);
            fixture.connected.set(false);
        }
        if (!emptyBatch) {
            fixture.dropData.set(record -> true);
            fixture.relay(R1, R2);
        }
        if (replaceAt == 0) fixture.routeGeneration.set(2);
        if (replaceAt == 3) fixture.connected.set(true);
        fixture.publish();
        if (replaceAt == 1) fixture.routeGeneration.set(2);
        fixture.management.clear();
        fixture.dropData.set(record -> false);
        fixture.dropCutover.set(false);
        fixture.connected.set(readyAtObservation);
        var settlement =
                fixture.source
                        .settleAuthority(
                                fixture.targetRid,
                                fixture.request.fence(),
                                Instant.now().plusSeconds(30))
                        .toCompletableFuture();
        if (replaceAt == 2) fixture.routeGeneration.set(2);
        if (!settlement.isDone()) fixture.nextManagementTurn(settlement);
        if (!readyAtObservation) {
            assertFalse(settlement.isDone(), "a selected but unready route cannot carry replay");
            fixture.connected.set(true);
            fixture.nextManagementTurn(settlement);
        }
        fixture.endpoint.publishedEvent.get(3, TimeUnit.SECONDS);
        assertEquals(1, fixture.commits.get(), "the replay must deliver the verified boundary");
        // Resubmission and the source's later Store confirmation are distinct observations
        // (relocation §4.4). Await another authority turn or that confirmation.
        if (!settlement.isDone()) fixture.nextManagementTurn(settlement);
        // The turn submits the Store read; its completion may arrive on another thread.
        settlement.get(3, TimeUnit.SECONDS);
        assertTrue(settlement.isDone(), "the source must observe the target's committed authority");
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED,
                settlement.join().outcome());
        var target = settlement.join().target();
        assertNotNull(target, "target settlement must retain the exact authority snapshot");
        assertEquals(fixture.targetOwner.ownerId(), target.ownerId());
        assertEquals(fixture.targetOwner.leaseGeneration(), target.ownerLeaseGeneration());
        assertEquals(
                fixture.authority().authorityOwnerGeneration(), target.authorityOwnerGeneration());
        assertEquals(emptyBatch ? 0 : 2, fixture.endpoint.relayed().size());
        assertEquals(1, fixture.commits.get());
    }

    @Test
    void completedSettlementWakesManagementObservationWithoutAnotherTurn() throws Exception {
        Fixture fixture = new Fixture(true);
        fixture.stage();
        fixture.publish();
        fixture.endpoint.publishedEvent.get(3, TimeUnit.SECONDS);
        var settlement =
                fixture.source
                        .settleAuthority(
                                fixture.targetRid,
                                fixture.request.fence(),
                                Instant.now().plusSeconds(30))
                        .toCompletableFuture();
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED,
                settlement.get(3, TimeUnit.SECONDS).outcome());
        assertTrue(fixture.management.isEmpty(), "Store confirmation must not need another turn");
        fixture.nextManagementTurn(settlement);
        assertTrue(fixture.management.isEmpty(), "the settlement wakeup must be consumed");
        assertEquals(1, fixture.commits.get());
    }

    @Test
    void shutdownWaitsForStageInstallationBeforeAbort() throws Exception {
        Fixture fixture = new Fixture();
        fixture.endpoint.stageCompletion = new CompletableFuture<>();
        var staging =
                fixture.source
                        .stage(
                                fixture.targetRid,
                                fixture.request,
                                Duration.ofSeconds(2),
                                STAGE_DEADLINE)
                        .toCompletableFuture();
        fixture.endpoint.stageEntered.get(3, TimeUnit.SECONDS);
        var terminal = fixture.target.sealAcceptedTargetRelocations().toCompletableFuture();
        assertEquals(0, fixture.endpoint.abortedBeforeInstalled.get());
        assertEquals(0, fixture.endpoint.aborted.get());
        assertFalse(terminal.isDone());
        fixture.endpoint.stageCompletion.complete(null);
        terminal.get(3, TimeUnit.SECONDS);
        staging.handle((ignored, failure) -> null).get(3, TimeUnit.SECONDS);
        assertEquals(1, fixture.endpoint.aborted.get());
        assertEquals(1, fixture.failedReplies.get());
        assertEquals(0, fixture.commits.get());
        assertEquals(0, fixture.endpoint.published.get());
    }

    @Test
    void reentrantShutdownAtStageEntryWaitsForInstallation() throws Exception {
        Fixture fixture = new Fixture();
        fixture.endpoint.stageCompletion = new CompletableFuture<>();
        AtomicReference<CompletionStage<Void>> terminal = new AtomicReference<>();
        var shutdownEntered =
                fixture.endpoint.stageEntered.thenRun(
                        () -> terminal.set(fixture.target.sealAcceptedTargetRelocations()));
        var staging =
                fixture.source
                        .stage(
                                fixture.targetRid,
                                fixture.request,
                                Duration.ofSeconds(2),
                                STAGE_DEADLINE)
                        .toCompletableFuture();
        shutdownEntered.get(3, TimeUnit.SECONDS);
        assertNotNull(terminal.get());
        assertFalse(terminal.get().toCompletableFuture().isDone());
        assertEquals(0, fixture.endpoint.abortedBeforeInstalled.get());
        fixture.endpoint.stageCompletion.complete(null);
        terminal.get().toCompletableFuture().get(3, TimeUnit.SECONDS);
        staging.handle((ignored, failure) -> null).get(3, TimeUnit.SECONDS);
        assertEquals(1, fixture.endpoint.aborted.get());
        assertEquals(0, fixture.commits.get());
    }

    @Test
    void shutdownWaitsForAcceptedStorePrepareBeforeDiscardingInstalledStage() throws Exception {
        Fixture fixture = new Fixture();
        fixture.prepareCompletion = new CompletableFuture<>();
        var staging =
                fixture.source
                        .stage(
                                fixture.targetRid,
                                fixture.request,
                                Duration.ofSeconds(2),
                                STAGE_DEADLINE)
                        .toCompletableFuture();
        fixture.prepareEntered.get(3, TimeUnit.SECONDS);
        var terminal = fixture.target.sealAcceptedTargetRelocations().toCompletableFuture();
        assertFalse(terminal.isDone());
        assertEquals(0, fixture.endpoint.aborted.get());
        fixture.prepareCompletion.complete(null);
        terminal.get(3, TimeUnit.SECONDS);
        staging.handle((ignored, failure) -> null).get(3, TimeUnit.SECONDS);
        assertEquals(1, fixture.endpoint.aborted.get());
        assertEquals(0, fixture.commits.get());
    }

    @Test
    void failedStageAfterShutdownDoesNotAbortMissingStage() throws Exception {
        Fixture fixture = new Fixture();
        fixture.endpoint.stageCompletion = new CompletableFuture<>();
        var staging =
                fixture.source
                        .stage(
                                fixture.targetRid,
                                fixture.request,
                                Duration.ofSeconds(2),
                                STAGE_DEADLINE)
                        .toCompletableFuture();
        fixture.endpoint.stageEntered.get(3, TimeUnit.SECONDS);
        var terminal = fixture.target.sealAcceptedTargetRelocations().toCompletableFuture();
        fixture.endpoint.stageCompletion.completeExceptionally(
                new IllegalStateException("restore failed"));
        terminal.get(3, TimeUnit.SECONDS);
        assertThrows(CompletionException.class, staging::join);
        assertEquals(0, fixture.endpoint.abortedBeforeInstalled.get());
        assertEquals(0, fixture.endpoint.aborted.get());
        assertEquals(0, fixture.commits.get());
    }

    @Test
    void failedStageWithoutShutdownSettlesThroughTheSameTerminalOwner() throws Exception {
        Fixture fixture = new Fixture();
        fixture.endpoint.stageCompletion =
                CompletableFuture.failedFuture(new IllegalStateException("restore failed"));
        assertThrows(CompletionException.class, fixture::stage);
        fixture.target
                .awaitAcceptedTargetRelocations()
                .toCompletableFuture()
                .get(3, TimeUnit.SECONDS);
        assertEquals(0, fixture.endpoint.abortedBeforeInstalled.get());
        assertEquals(0, fixture.endpoint.aborted.get());
        assertEquals(0, fixture.commits.get());
    }

    @Test
    void targetPrepareFailureReportsOriginalCauseAndIdentityOnce() throws Exception {
        ZLinkDispatchOptionsRegistration dispatchOptions = new ZLinkDispatchOptionsRegistration();
        dispatchOptions.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        ZLinkTelemetryOwner.Host telemetryHost = ZLinkTelemetryOwner.openHost();
        var activator =
                ZLinkHandlerActivator.services().add(ZLinkTelemetryOwner.Host.class, telemetryHost);
        ZLinkDispatchErrorReporter dispatchErrors =
                new ZLinkDispatchErrorReporter(dispatchOptions, activator, Runnable::run);

        List<String> records = new CopyOnWriteArrayList<>();
        CompletableFuture<String> dispatchErrorRecord = new CompletableFuture<>();
        Logger logger =
                Logger.getLogger(
                        systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer.class
                                .getName());
        boolean useParentHandlers = logger.getUseParentHandlers();
        Level previousLevel = logger.getLevel();
        Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        String message = record.getMessage();
                        records.add(message);
                        if (message.contains("event_id=zlink.dispatch_error")) {
                            dispatchErrorRecord.complete(message);
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(handler);
        logger.setUseParentHandlers(false);
        logger.setLevel(Level.ALL);
        try {
            Fixture successful = new Fixture(false, dispatchErrors);
            successful.stage();
            assertEquals(0L, dispatchErrors.reportedCount());
            assertEquals(0L, dispatchErrors.flow().tracedCount());

            Fixture failed = new Fixture(false, dispatchErrors);
            failed.endpoint.stageCompletion = new CompletableFuture<>();
            var staging = failed.startStage();
            failed.endpoint.stageEntered.get(3, TimeUnit.SECONDS);
            assertFalse(
                    staging.isDone(), "target must not be relay-ready before Restore completes");
            failed.endpoint.stageCompletion.completeExceptionally(
                    new IllegalStateException("restore adapter rejected state"));
            assertThrows(CompletionException.class, staging::join);
            failed.target
                    .awaitAcceptedTargetRelocations()
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);

            assertEquals(1, failed.failedReplies.get());
            assertEquals(List.of(17L), failed.failedFailureCodes);
            assertEquals(1L, dispatchErrors.reportedCount());
            assertEquals(1L, dispatchErrors.flow().tracedCount());
            String event = dispatchErrorRecord.get(3, TimeUnit.SECONDS);
            assertEquals(1, records.size());
            assertTrue(event.contains("mesh=mesh"), event);
            assertTrue(event.contains("source_rid=source-node"), event);
            assertTrue(event.contains("target_rid=target-node"), event);
            assertTrue(event.contains("actor=actor-settlement"), event);
            assertTrue(
                    event.contains(
                            "corr="
                                    + failed.request.fence().aggregateId()
                                    + "/attempt/"
                                    + failed.targetAttemptGeneration.get()),
                    event);
            assertTrue(event.contains("packet=target_prepare_restore"), event);
            assertTrue(event.contains("error_type=IllegalStateException"), event);
            assertTrue(event.contains("error_message=restore adapter rejected state"), event);
        } finally {
            ZLinkTelemetryTestFence.awaitIdle();
            telemetryHost.close();
            logger.removeHandler(handler);
            logger.setUseParentHandlers(useParentHandlers);
            logger.setLevel(previousLevel);
        }
    }

    @Test
    void failedStorePrepareDiscardsTheInstalledStageOnce() throws Exception {
        Fixture fixture = new Fixture();
        fixture.prepareCompletion =
                CompletableFuture.failedFuture(new IllegalStateException("Store prepare failed"));
        assertThrows(CompletionException.class, fixture::stage);
        fixture.target
                .awaitAcceptedTargetRelocations()
                .toCompletableFuture()
                .get(3, TimeUnit.SECONDS);
        assertEquals(1, fixture.endpoint.aborted.get());
        assertEquals(0, fixture.endpoint.abortedBeforeInstalled.get());
        assertFalse(fixture.endpoint.installed);
        assertEquals(1, fixture.failedReplies.get());
        assertEquals(0, fixture.commits.get());
    }

    @Test
    void shutdownBeforePayloadAssemblyTerminatesWithoutInstallingAStage() throws Exception {
        Fixture fixture = new Fixture();
        fixture.stateDelivery = new CompletableFuture<>();
        var staging =
                fixture.source
                        .stage(
                                fixture.targetRid,
                                fixture.request,
                                Duration.ofSeconds(2),
                                STAGE_DEADLINE)
                        .toCompletableFuture();
        fixture.stateEntered.get(3, TimeUnit.SECONDS);
        fixture.target
                .sealAcceptedTargetRelocations()
                .toCompletableFuture()
                .get(3, TimeUnit.SECONDS);
        fixture.stateDelivery.complete(null);
        assertThrows(CompletionException.class, staging::join);
        assertFalse(fixture.endpoint.stageEntered.isDone());
        assertEquals(0, fixture.endpoint.aborted.get());
        assertEquals(0, fixture.commits.get());
    }

    @Test
    void shutdownAfterVerifiedCutoverWaitsForCasSettlement() throws Exception {
        Fixture fixture = new Fixture();
        fixture.endpoint.relayCompletion = new CompletableFuture<>();
        fixture.stageAndRelay(R1);
        try (var runtime = fixture.startTargetHost()) {
            var publication =
                    fixture.source
                            .publish(
                                    fixture.targetRid,
                                    fixture.request.fence(),
                                    Duration.ofSeconds(2))
                            .toCompletableFuture();
            assertEquals(1, fixture.endpoint.relayed().size());
            assertEquals(0, fixture.commits.get(), "verification precedes the CAS");
            var shutdown = runtime.shutdown(Duration.ofSeconds(1)).toCompletableFuture();
            assertEquals(0, fixture.endpoint.aborted.get());
            assertFalse(shutdown.isDone());
            fixture.endpoint.relayCompletion.complete(null);
            publication.get(3, TimeUnit.SECONDS);
            assertEquals(
                    ZLinkFrameworkTerminationOutcome.STOPPED,
                    shutdown.get(3, TimeUnit.SECONDS).outcome());
            assertEquals(1, fixture.commits.get());
            assertEquals(1, fixture.endpoint.published.get());
            assertEquals(0, fixture.endpoint.aborted.get());
            assertEquals(fixture.targetOwner.ownerId(), fixture.authority().ownerId());
        }
    }

    @Test
    void shutdownAfterIndeterminateCasWaitsForAuthorityReconciliation() throws Exception {
        Fixture fixture = new Fixture();
        fixture.ambiguousCommit.set(true);
        fixture.stageAndRelay(R1);
        try (var runtime = fixture.startTargetHost()) {
            var publication =
                    fixture.source
                            .publish(
                                    fixture.targetRid,
                                    fixture.request.fence(),
                                    Duration.ofSeconds(2))
                            .toCompletableFuture();
            fixture.reconciliationEntered.get(3, TimeUnit.SECONDS);
            assertEquals(1, fixture.commits.get());
            assertEquals(
                    fixture.targetOwner.ownerId(),
                    fixture.authority().ownerId(),
                    "the CAS succeeded but its response was lost");
            var shutdown = runtime.shutdown(Duration.ofSeconds(1)).toCompletableFuture();
            assertEquals(0, fixture.endpoint.aborted.get());
            assertFalse(shutdown.isDone());
            fixture.reconciliationRelease.complete(null);
            publication.get(3, TimeUnit.SECONDS);
            assertEquals(
                    ZLinkFrameworkTerminationOutcome.STOPPED,
                    shutdown.get(3, TimeUnit.SECONDS).outcome());
            assertEquals(1, fixture.endpoint.published.get());
            assertEquals(0, fixture.endpoint.aborted.get());
        }
    }

    @Test
    void shutdownTerminatesAcceptedTargetBeforeCutover() throws Exception {
        Fixture fixture = new Fixture();
        fixture.dropCutover.set(true);
        fixture.stageAndRelay(R1);
        assertThrows(CompletionException.class, fixture::publish);
        var terminal = fixture.target.awaitAcceptedTargetRelocations().toCompletableFuture();
        try (var runtime = fixture.startTargetHost()) {
            try {
                var result =
                        runtime.shutdown(Duration.ofSeconds(1))
                                .toCompletableFuture()
                                .get(3, TimeUnit.SECONDS);
                assertTrue(
                        terminal.isDone(),
                        "shutdown left the accepted target attempt nonterminal: " + result);
                assertEquals(ZLinkFrameworkTerminationOutcome.STOPPED, result.outcome());
                assertEquals(1, fixture.endpoint.aborted.get());
                fixture.target
                        .apply(
                                fixture.sourceRid,
                                ServiceWireConstants.COMMAND_RELOCATION_CUTOVER,
                                fixture.cutover.get())
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);
                assertEquals(0, fixture.commits.get(), "seal prevents a late CUTOVER CAS");
                assertEquals(0, fixture.endpoint.published.get());
                assertEquals(fixture.sourceOwner.ownerId(), fixture.authority().ownerId());
            } finally {
                fixture.locations
                        .releaseOwnerLease(fixture.targetOwner)
                        .toCompletableFuture()
                        .join();
                fixture.cutoverWarning.get().run();
                terminal.get(3, TimeUnit.SECONDS);
            }
        }
    }

    @Test
    void cutoverWaitIsAWarningOnlyAndNeverStartsTheTargetCas() throws Exception {
        Fixture fixture = new Fixture();
        fixture.dropCutover.set(true);
        fixture.stageAndRelay(R1);

        assertThrows(CompletionException.class, fixture::publish);
        fixture.cutoverWarning.get().run();

        assertEquals(0, fixture.commits.get(), "no target CAS without a verified CUTOVER");
        assertEquals(0, fixture.endpoint.published.get(), "dispatch stays closed");
        assertEquals(List.of(), fixture.endpoint.relayed(), "relay is staged only after CUTOVER");
    }

    @Test
    void resentWholeBatchReplacesThePartialSectionBeforeTheVerifiedCutover() throws Exception {
        Fixture fixture = new Fixture();
        fixture.dropCutover.set(true);
        fixture.dropData.set(record -> record[0] == R2[0]);
        fixture.stageAndRelay(R1, R2);
        assertThrows(CompletionException.class, fixture::publish);

        //  The new connection carries the whole pre-boundary batch, then the cutover.
        fixture.applyToTarget(ServiceWireConstants.COMMAND_RELOCATION_DATA, fixture.data.get(0));
        fixture.applyToTarget(ServiceWireConstants.COMMAND_RELOCATION_DATA, fixture.data.get(1));
        fixture.applyToTarget(
                ServiceWireConstants.COMMAND_RELOCATION_CUTOVER, fixture.cutover.get());

        fixture.endpoint.publishedEvent.get(3, TimeUnit.SECONDS);
        assertEquals(1, fixture.commits.get());
        List<byte[]> relayed = fixture.endpoint.relayed();
        assertEquals(2, relayed.size(), "each pre-boundary record is staged once, in order");
        assertArrayEquals(R1, relayed.get(0));
        assertArrayEquals(R2, relayed.get(1));
    }

    @Test
    void sourcePreserveFenceWinsAtTheRestoreDeadlineAndTheTargetDiscardsStaging() throws Exception {
        Fixture fixture = new Fixture();
        fixture.dropCutover.set(true);
        fixture.stageAndRelay(R1);
        assertThrows(CompletionException.class, fixture::publish);
        String versionBefore = fixture.authority().storeVersion();

        ZLinkRelocationTransitionClient.Settlement settlement =
                fixture.source
                        .settle(fixture.targetRid, fixture.request.fence(), Instant.now())
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);

        assertEquals(ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED, settlement);
        ZLinkAuthoritySnapshot after = fixture.authority();
        assertEquals(fixture.sourceOwner.ownerId(), after.ownerId(), "the source stays owner");
        assertNotEquals(versionBefore, after.storeVersion(), "Preserve changes the StoreVersion");
        fixture.cutoverWarning.get().run();
        fixture.endpoint.abortedEvent.get(3, TimeUnit.SECONDS);

        fixture.applyToTarget(
                ServiceWireConstants.COMMAND_RELOCATION_CUTOVER, fixture.cutover.get());
        assertEquals(0, fixture.commits.get(), "a late CUTOVER does not change settled authority");
        assertEquals(0, fixture.endpoint.published.get());
    }

    @Test
    void sourceResendsTheWholeBatchAndCutoverAfterReconnectAndConfirmsTheTargetCommit()
            throws Exception {
        Fixture fixture = new Fixture();
        fixture.stage();
        fixture.connected.set(false);
        fixture.dropData.set(record -> true);
        fixture.dropCutover.set(true);
        fixture.relay(R1, R2);
        assertThrows(CompletionException.class, fixture::publish);

        CompletableFuture<ZLinkRelocationTransitionClient.Settlement> settlement =
                fixture.source
                        .settle(
                                fixture.targetRid,
                                fixture.request.fence(),
                                Instant.now().plusSeconds(30))
                        .toCompletableFuture();
        assertEquals(0, fixture.commits.get());

        fixture.dropData.set(record -> false);
        fixture.dropCutover.set(false);
        fixture.connected.set(true);

        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED,
                settlement.get(3, TimeUnit.SECONDS));
        assertEquals(1, fixture.commits.get());
        List<byte[]> relayed = fixture.endpoint.relayed();
        assertEquals(2, relayed.size());
        assertArrayEquals(R1, relayed.get(0));
        assertArrayEquals(R2, relayed.get(1));
        assertEquals(fixture.targetOwner.ownerId(), fixture.authority().ownerId());
    }

    @RepeatedTest(20)
    void targetCommitThenAuthorityRemovalEndsSourceWithoutPreserve() throws Exception {
        Fixture fixture = new Fixture();
        fixture.stageAndRelay(R1);
        fixture.publish();
        fixture.endpoint.publishedEvent.get(3, TimeUnit.SECONDS);
        var committed = fixture.authority();
        assertEquals(fixture.targetOwner.ownerId(), committed.ownerId());
        assertInstanceOf(
                ZLinkAuthorityDeleted.class,
                fixture.locations
                        .compareExchange(
                                fixture.authorityKey,
                                new ZLinkAuthorityExpectFound(committed.storeVersion()),
                                new ZLinkAuthorityDelete(),
                                OPEN)
                        .toCompletableFuture()
                        .join());
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_LEASE_EXPIRED,
                fixture.source
                        .settle(
                                fixture.targetRid,
                                fixture.request.fence(),
                                Instant.now().plusSeconds(30))
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS));
        assertInstanceOf(
                ZLinkAuthorityMissing.class,
                fixture.locations.read(fixture.authorityKey, OPEN).toCompletableFuture().join());
        assertEquals(1, fixture.commits.get());
    }

    @Test
    void expiredSourceLeaseEndsSettlementBeforeTheRestoreDeadline() throws Exception {
        Fixture fixture = new Fixture();
        fixture.dropCutover.set(true);
        fixture.stageAndRelay(R1);
        assertThrows(CompletionException.class, fixture::publish);
        fixture.locations.releaseOwnerLease(fixture.sourceOwner).toCompletableFuture().join();

        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_LEASE_EXPIRED,
                fixture.source
                        .settle(
                                fixture.targetRid,
                                fixture.request.fence(),
                                Instant.now().plusSeconds(30))
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS));
        assertEquals(0, fixture.commits.get());
    }

    @Test
    void continuedRelocationProvesCommitWithoutTheFirstTargetOwner() throws Exception {
        Fixture fixture = new Fixture();
        fixture.stage();
        var source = fixture.authority();
        fixture.authorityRead.set(
                new ZLinkAuthoritySnapshot(
                        "next-owner-version",
                        source.payload(),
                        source.objectGeneration(),
                        source.authorityOwnerGeneration() + 2,
                        "next-owner",
                        1,
                        source.allocation(),
                        source.storeNow()));
        var settled =
                fixture.source
                        .settleAuthority(fixture.targetRid, fixture.request.fence(), Instant.now())
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED, settled.outcome());
        org.junit.jupiter.api.Assertions.assertNull(
                settled.target(),
                "a subsequent owner does not reveal the first target's Follow fence");
    }

    @Test
    void replacementObjectOnTargetDoesNotProveThisRelocationCommitted() throws Exception {
        Fixture fixture = new Fixture();
        fixture.stage();
        var source = fixture.authority();
        fixture.authorityRead.set(
                new ZLinkAuthoritySnapshot(
                        "replacement-version",
                        source.payload(),
                        source.objectGeneration() + 1,
                        source.authorityOwnerGeneration() + 1,
                        fixture.targetOwner.ownerId(),
                        fixture.targetOwner.leaseGeneration(),
                        source.allocation(),
                        source.storeNow()));
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_LEASE_EXPIRED,
                fixture.source
                        .settle(fixture.targetRid, fixture.request.fence(), Instant.now())
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS));
    }
}
