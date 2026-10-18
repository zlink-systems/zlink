package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;

import java.io.IOException;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;

/** Host relocation §13: applied descriptor writes require confirmed Serving rollback. */
final class ZLinkRelocationPublicationFailureTest {
    private static final List<String> MESHES = List.of("publication-a", "publication-b");
    private static final Duration WAIT = Duration.ofSeconds(15);

    enum Fault {
        STORE_REPLY_LOST,
        TYPED_VALIDATION,
        ROLLBACK_REPLY_LOST,
        EXTERNAL_SHUTDOWN_WINS
    }

    @ParameterizedTest
    @EnumSource(Fault.class)
    void publicationFailureWaitsForRollbackAndNeverReopensAnUnconfirmedHost(Fault fault)
            throws Exception {
        var store = new AppliedWriteStore();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRelocationStore(new InMemoryRelocationStore());
        for (String mesh : MESHES) {
            options.addRouteMesh(mesh)
                    .listen("tcp://127.0.0.1:0")
                    .setRoutingId(RoutingId.from(mesh));
        }
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(WAIT.toSeconds(), TimeUnit.SECONDS);
            assertEquals(2, store.descriptors().size());
            store.armed.set(true);
            var relocation =
                    runtime.relocate(
                                    new ZLinkFrameworkRelocationOptions(
                                            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                            null,
                                            WAIT))
                            .toCompletableFuture();
            try {
                store.relocatingApplied.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertTrue(
                        store.descriptors().stream()
                                .allMatch(d -> d.state() == ZLinkFrameworkRuntimeState.RELOCATING));
                store.publicationFailed.set(true);
                store.publicationReply.completeExceptionally(
                        fault == Fault.TYPED_VALIDATION
                                ? new IllegalArgumentException("injected provider validation")
                                : new IOException("descriptor applied but reply lost"));
                CompletableFuture.anyOf(store.servingApplied, relocation)
                        .get(WAIT.toSeconds(), TimeUnit.SECONDS);
                assertTrue(
                        store.servingApplied.isDone(), "all attempted descriptors need rollback");
                if (fault != Fault.ROLLBACK_REPLY_LOST && fault != Fault.EXTERNAL_SHUTDOWN_WINS) {
                    assertEquals(
                            ZLinkFrameworkRuntimeState.RELOCATING,
                            runtime.status().state(),
                            "source admission must stay closed until rollback replies confirm");
                    assertFalse(relocation.isDone(), "rollback submission is not confirmation");
                    store.rollbackReply.complete(null);
                    var result = relocation.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    assertEquals(ZLinkFrameworkRelocationOutcome.BLOCKED, result.outcome());
                    assertEquals(
                            fault == Fault.TYPED_VALIDATION
                                    ? ZLinkFrameworkRelocationReason.RELOCATION_FAILED
                                    : ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE,
                            result.reason());
                    assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.status().state());
                    assertTrue(
                            store.descriptors().stream()
                                    .allMatch(
                                            d -> d.state() == ZLinkFrameworkRuntimeState.SERVING));
                } else if (fault == Fault.EXTERNAL_SHUTDOWN_WINS) {
                    // Later descriptor writes belong to Shutdown, not the held rollback.
                    store.armed.set(false);
                    var shutdown = runtime.shutdown(WAIT).toCompletableFuture();
                    var shutdownResult = shutdown.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    assertEquals(
                            ZLinkFrameworkTerminationOutcome.STOPPED, shutdownResult.outcome());
                    assertEquals(ZLinkFrameworkTerminationReason.NONE, shutdownResult.reason());
                    store.rollbackReply.completeExceptionally(
                            new IOException("rollback failed after Shutdown took admission"));
                    var result = relocation.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    assertEquals(
                            ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED, result.reason());
                    assertEquals(
                            shutdownResult, runtime.status().terminationResult().orElseThrow());
                    assertEquals(
                            shutdownResult, runtime.shutdown(WAIT).toCompletableFuture().join());
                    assertEquals(ZLinkFrameworkRuntimeState.STOPPED, runtime.status().state());
                } else {
                    store.armed.set(false);
                    store.rollbackReply.completeExceptionally(
                            new IOException("Serving rollback reply lost"));
                    relocation.get(WAIT.toSeconds(), TimeUnit.SECONDS);
                    var terminal = runtime.status().terminationResult().orElseThrow();
                    assertEquals(
                            ZLinkFrameworkTerminationOutcome.FORCE_STOPPED, terminal.outcome());
                    assertEquals(
                            ZLinkFrameworkTerminationReason.TEARDOWN_FAILED, terminal.reason());
                    assertEquals(ZLinkFrameworkRuntimeState.STOPPED, runtime.status().state());
                    assertFalse(runtime.status().acceptingWork());
                }
            } finally {
                // Every held provider operation is released even when an assertion fails.
                store.armed.set(false);
                store.publicationReply.complete(null);
                store.rollbackReply.complete(null);
            }
        }
    }

    @Test
    void publicShutdownStillRejectsZeroBudget() {
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(new DefaultZLinkFrameworkOptions())) {
            assertThrows(IllegalArgumentException.class, () -> runtime.shutdown(Duration.ZERO));
        }
    }

    @Test
    @SuppressWarnings("unchecked")
    void expiredInternalBudgetSealsAdmissionAndUsesBoundedTeardown() throws Exception {
        var runtime = ZLinkFrameworkRuntimeTestAccess.start(new DefaultZLinkFrameworkOptions());
        try {
            var terminate =
                    ZLinkFrameworkRuntime.class.getDeclaredMethod(
                            "beginTermination", Duration.class, ZLinkTerminationReason.class);
            terminate.setAccessible(true);
            var completion =
                    (CompletionStage<ZLinkTerminationResult>)
                            terminate.invoke(
                                    runtime, Duration.ZERO, ZLinkTerminationReason.TEARDOWN_FAILED);
            assertFalse(runtime.status().acceptingWork());
            var terminal = completion.toCompletableFuture().get(WAIT.toSeconds(), TimeUnit.SECONDS);
            assertEquals(ZLinkTerminationOutcome.FORCE_STOPPED, terminal.outcome());
            assertEquals(ZLinkTerminationReason.TEARDOWN_FAILED, terminal.reason());
            assertEquals(ZLinkFrameworkRuntimeState.STOPPED, runtime.status().state());
        } finally {
            var cleanupFailure = assertThrows(ZLinkFrameworkShutdown.Failure.class, runtime::close);
            assertEquals("channel_close", cleanupFailure.stage());
            assertInstanceOf(TimeoutException.class, cleanupFailure.getCause());
        }
    }

    private static final class AppliedWriteStore implements ZLinkLocationStore {
        private static final String DESCRIPTOR_PREFIX = "mesh-node\0";
        final ZLinkInMemoryLocationStore inner = new ZLinkInMemoryLocationStore();
        final ZLinkProviderLocationRepository repository =
                new ZLinkProviderLocationRepository(inner);
        final AtomicBoolean armed = new AtomicBoolean();
        final AtomicBoolean publicationFailed = new AtomicBoolean();
        final CompletableFuture<Void> publicationReply = new CompletableFuture<>();
        final CompletableFuture<Void> rollbackReply = new CompletableFuture<>();
        final CompletableFuture<Void> relocatingApplied = new CompletableFuture<>();
        final CompletableFuture<Void> servingApplied = new CompletableFuture<>();
        final java.util.Set<String> relocatingMeshes = ConcurrentHashMap.newKeySet();
        final java.util.Set<String> servingMeshes = ConcurrentHashMap.newKeySet();
        final java.util.Set<ZLinkStoreKey> failedReconciliationReads =
                ConcurrentHashMap.newKeySet();

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            if (failedReconciliationReads.remove(key))
                return CompletableFuture.failedFuture(
                        new IOException("descriptor reconciliation unavailable"));
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return inner.write(request, cancellation)
                    .thenCompose(
                            result -> {
                                var descriptorKey =
                                        request.mutations().stream()
                                                .filter(
                                                        m ->
                                                                m instanceof ZLinkStorePut put
                                                                        && put.key()
                                                                                .value()
                                                                                .startsWith(
                                                                                        DESCRIPTOR_PREFIX))
                                                .map(m -> ((ZLinkStorePut) m).key())
                                                .findFirst();
                                if (!armed.get() || descriptorKey.isEmpty())
                                    return CompletableFuture.completedFuture(result);
                                var descriptors = descriptors();
                                boolean rollback = publicationFailed.get();
                                var meshes = rollback ? servingMeshes : relocatingMeshes;
                                var expected =
                                        rollback
                                                ? ZLinkFrameworkRuntimeState.SERVING
                                                : ZLinkFrameworkRuntimeState.RELOCATING;
                                for (var descriptor : descriptors) {
                                    if (descriptor.state() == expected)
                                        meshes.add(descriptor.meshName());
                                }
                                if (meshes.containsAll(MESHES))
                                    (rollback ? servingApplied : relocatingApplied).complete(null);
                                return (rollback ? rollbackReply : publicationReply)
                                        .handle(
                                                (ignored, failure) -> {
                                                    if (failure != null) {
                                                        // The repository correctly reconciles a
                                                        // lost write reply.
                                                        // Make that read fail too, preserving the
                                                        // original error.
                                                        failedReconciliationReads.add(
                                                                descriptorKey.orElseThrow());
                                                        throw new CompletionException(failure);
                                                    }
                                                    return result;
                                                });
                            });
        }

        List<ZLinkMeshNodeDescriptor> descriptors() {
            return MESHES.stream()
                    .flatMap(
                            mesh ->
                                    repository
                                            .listMeshNodes(mesh, ZLinkPageRequest.firstPage())
                                            .toCompletableFuture()
                                            .join()
                                            .items()
                                            .stream())
                    .toList();
        }
    }
}
