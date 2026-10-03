package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

final class ZLinkFrameworkShutdownTest {
    @Test
    void socketCloseFailuresReachTheCallerForSynchronousAndAsyncCleanup() {
        for (boolean async : List.of(false, true)) {
            var shutdown =
                    new ZLinkFrameworkShutdown(
                            Instant.now().plus(ZLinkFrameworkRuntime.DEFAULT_TERMINATION_DEADLINE));
            var failure =
                    new systems.zlink.contracts.errors.ZlinkCloseException(
                            systems.zlink.contracts.errors.CloseResult.BUSY, 16);
            var remaining = new ArrayList<String>();
            shutdown.defer("remaining", () -> remaining.add("closed"));
            if (async) {
                shutdown.deferCloseStage(
                        "socket_close", () -> CompletableFuture.failedFuture(failure));
            } else {
                shutdown.defer(
                        "socket_close",
                        () -> {
                            throw failure;
                        });
            }
            var thrown =
                    assertThrows(
                            CompletionException.class,
                            () -> shutdown.closeAsync().toCompletableFuture().join());
            var stage = assertInstanceOf(ZLinkFrameworkShutdown.Failure.class, thrown.getCause());
            assertEquals("socket_close", stage.stage());
            assertSame(failure, stage.getCause());
            assertEquals(List.of("closed"), remaining);
        }
    }

    @Test
    void expiredHostDeadlineStartsAsyncResourcesAndRejectsNewStoreWork() {
        var shutdown = new ZLinkFrameworkShutdown(Instant.EPOCH);
        var resources = new ArrayList<String>();
        var stores = new ArrayList<String>();
        var providers = new ArrayList<CompletableFuture<Void>>();
        for (String stage : List.of("location_stop", "descriptor_remove")) {
            shutdown.deferStage(
                    stage,
                    () -> {
                        stores.add(stage);
                        return CompletableFuture.completedFuture(null);
                    });
        }
        for (String stage :
                List.of("spot_close", "auto_connect_stop", "instance_close", "stream_close")) {
            var provider = new CompletableFuture<Void>();
            providers.add(provider);
            shutdown.deferCloseStage(
                    stage,
                    () -> {
                        resources.add(stage);
                        return provider;
                    });
        }
        var thrown =
                assertThrows(
                        CompletionException.class,
                        () -> shutdown.closeAsync().toCompletableFuture().join());
        var failure = assertInstanceOf(ZLinkFrameworkShutdown.Failure.class, thrown.getCause());
        assertEquals("stream_close", failure.stage());
        assertInstanceOf(TimeoutException.class, failure.getCause());
        assertEquals(
                List.of("stream_close", "instance_close", "auto_connect_stop", "spot_close"),
                resources);
        assertEquals(List.of(), stores);
        assertEquals(4, providers.size());
        providers.forEach(provider -> assertFalse(provider.isDone()));
    }

    @Test
    void expiredHostDeadlineRejectsStoreSubmissionAndStillClosesResources() throws Exception {
        var shutdown = new ZLinkFrameworkShutdown(Instant.EPOCH);
        var calls = new ArrayList<String>();
        var contextClosed = new CompletableFuture<Void>();
        shutdown.defer(
                "context_close",
                () -> {
                    calls.add("context");
                    contextClosed.complete(null);
                });
        shutdown.deferStage(
                "descriptor_remove",
                () -> {
                    calls.add("descriptor");
                    return CompletableFuture.completedFuture(null);
                });
        var thrown =
                assertThrows(
                        CompletionException.class,
                        () -> shutdown.closeAsync().toCompletableFuture().join());
        var failure = assertInstanceOf(ZLinkFrameworkShutdown.Failure.class, thrown.getCause());
        assertEquals("descriptor_remove", failure.stage());
        assertInstanceOf(TimeoutException.class, failure.getCause());
        contextClosed.get(1, TimeUnit.SECONDS);
        assertEquals(List.of("context"), calls);
    }

    @Test
    void liveHostDeadlineSubmitsAndObservesTheStoreCompletion() {
        var shutdown =
                new ZLinkFrameworkShutdown(
                        Instant.now().plus(ZLinkFrameworkRuntime.DEFAULT_TERMINATION_DEADLINE));
        var calls = new ArrayList<String>();
        var provider = new CompletableFuture<Void>();
        shutdown.deferStage(
                "descriptor_remove",
                () -> {
                    calls.add("descriptor");
                    return provider;
                });
        var closing = shutdown.closeAsync().toCompletableFuture();
        assertEquals(List.of("descriptor"), calls);
        assertFalse(closing.isDone());
        assertFalse(provider.isDone());
        provider.complete(null);
        closing.join();
    }

    @Test
    void liveHostDeadlineObservesAsyncResourceCompletion() {
        var shutdown =
                new ZLinkFrameworkShutdown(
                        Instant.now().plus(ZLinkFrameworkRuntime.DEFAULT_TERMINATION_DEADLINE));
        var provider = new CompletableFuture<Void>();
        var calls = new ArrayList<String>();
        shutdown.deferCloseStage(
                "stream_close",
                () -> {
                    calls.add("stream");
                    return provider;
                });
        var closing = shutdown.closeAsync().toCompletableFuture();
        assertEquals(List.of("stream"), calls);
        assertFalse(closing.isDone());
        assertFalse(provider.isDone());
        provider.complete(null);
        closing.join();
    }

    @Test
    void cleanupPreservesTheFirstOwningStageAndContinuesAfterFailure() {
        var shutdown =
                new ZLinkFrameworkShutdown(
                        Instant.now().plus(ZLinkFrameworkRuntime.DEFAULT_TERMINATION_DEADLINE));
        var calls = new ArrayList<String>();
        var contextFailure = new IllegalStateException("context failed");
        var streamFailure = new IOException("stream failed\nwith detail");
        shutdown.deferCloseStage(
                "context_close",
                () -> {
                    calls.add("context");
                    return CompletableFuture.failedFuture(contextFailure);
                });
        shutdown.deferCloseStage(
                "stream_close",
                () -> {
                    calls.add("stream");
                    return CompletableFuture.failedFuture(streamFailure);
                });

        var thrown =
                assertThrows(
                        CompletionException.class,
                        () -> shutdown.closeAsync().toCompletableFuture().join());
        var failure = assertInstanceOf(ZLinkFrameworkShutdown.Failure.class, thrown.getCause());
        assertEquals(List.of("stream", "context"), calls);
        assertEquals("stream_close", failure.stage());
        assertSame(streamFailure, failure.getCause());
        assertEquals(1, failure.getSuppressed().length);
        var suppressed =
                assertInstanceOf(ZLinkFrameworkShutdown.Failure.class, failure.getSuppressed()[0]);
        assertEquals("context_close", suppressed.stage());
        assertSame(contextFailure, suppressed.getCause());
        assertEquals(0, failure.getStackTrace().length);
    }

    @Test
    void expiredHostDeadlineFailsTheOwningStageWithoutCompletingTheProviderFuture()
            throws Exception {
        var shutdown = new ZLinkFrameworkShutdown(Instant.EPOCH);
        var pending = new CompletableFuture<Void>();
        var calls = new ArrayList<String>();
        var contextClosed = new CompletableFuture<Void>();
        shutdown.defer(
                "context_close",
                () -> {
                    calls.add("context");
                    contextClosed.complete(null);
                });
        shutdown.deferStage("location_stop", () -> pending);
        var thrown =
                assertThrows(
                        CompletionException.class,
                        () -> shutdown.closeAsync().toCompletableFuture().join());
        var failure = assertInstanceOf(ZLinkFrameworkShutdown.Failure.class, thrown.getCause());
        assertEquals("location_stop", failure.stage());
        assertInstanceOf(TimeoutException.class, failure.getCause());
        contextClosed.get(1, TimeUnit.SECONDS);
        assertEquals(List.of("context"), calls);
        org.junit.jupiter.api.Assertions.assertFalse(pending.isDone());
    }

    @Test
    void synchronousCallbackFailureKeepsItsTypeAndStage() {
        var callbackFailure = new IllegalArgumentException("onClosing failed");
        var thrown =
                assertThrows(
                        CompletionException.class,
                        () ->
                                ZLinkFrameworkShutdown.atStage(
                                                "spot_close",
                                                () -> {
                                                    throw callbackFailure;
                                                })
                                        .toCompletableFuture()
                                        .join());
        var failure = assertInstanceOf(ZLinkFrameworkShutdown.Failure.class, thrown.getCause());
        assertEquals("spot_close", failure.stage());
        assertSame(callbackFailure, failure.getCause());
    }
}
