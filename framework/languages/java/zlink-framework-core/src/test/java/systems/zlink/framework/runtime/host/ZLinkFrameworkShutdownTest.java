package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;

final class ZLinkFrameworkShutdownTest {
    @Test
    void cleanupPreservesTheFirstOwningStageAndContinuesAfterFailure() {
        var shutdown = new ZLinkFrameworkShutdown();
        var calls = new ArrayList<String>();
        var contextFailure = new IllegalStateException("context failed");
        var streamFailure = new IOException("stream failed\nwith detail");
        shutdown.deferStage("context_close", () -> {
            calls.add("context");
            return CompletableFuture.failedFuture(contextFailure);
        });
        shutdown.deferStage("stream_close", () -> {
            calls.add("stream");
            return CompletableFuture.failedFuture(streamFailure);
        });

        var thrown = assertThrows(CompletionException.class,
            () -> shutdown.closeAsync().toCompletableFuture().join());
        var failure = assertInstanceOf(ZLinkFrameworkShutdown.Failure.class,
            thrown.getCause());
        assertEquals(List.of("stream", "context"), calls);
        assertEquals("stream_close", failure.stage());
        assertSame(streamFailure, failure.getCause());
        assertEquals(1, failure.getSuppressed().length);
        var suppressed = assertInstanceOf(ZLinkFrameworkShutdown.Failure.class,
            failure.getSuppressed()[0]);
        assertEquals("context_close", suppressed.stage());
        assertSame(contextFailure, suppressed.getCause());
        assertEquals(0, failure.getStackTrace().length);
    }

    @Test
    void synchronousCallbackFailureKeepsItsTypeAndStage() {
        var callbackFailure = new IllegalArgumentException("onClosing failed");
        var thrown = assertThrows(CompletionException.class, () ->
            ZLinkFrameworkShutdown.atStage("spot_close", () -> {
                throw callbackFailure;
            }).toCompletableFuture().join());
        var failure = assertInstanceOf(ZLinkFrameworkShutdown.Failure.class,
            thrown.getCause());
        assertEquals("spot_close", failure.stage());
        assertSame(callbackFailure, failure.getCause());
    }
}
