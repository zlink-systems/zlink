package systems.zlink.framework.runtime.internal;

import static org.junit.jupiter.api.Assertions.*;

import jdk.jfr.Recording;
import jdk.jfr.consumer.RecordingFile;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest;

import java.nio.file.Files;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;

/** Compares cancellation capture storage with controls in the same allocation JVM. */
public final class ZLinkCompletionCaptureAllocationTest {
    private static final int REQUESTS = 1000;
    private static final BiConsumer<CompletionStage<?>, CompletionStage<?>> CANCEL =
            (first, second) -> {
                first.toCompletableFuture().cancel(false);
                if (second != null) second.toCompletableFuture().cancel(false);
            };

    @Test
    void cancellationCapturesOnlyItsForwardingTargets() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    private static <T, U> void reference(
            CompletableFuture<?> caller, T first, U second, BiConsumer<T, U> action) {
        caller.whenComplete(
                (ignored, failure) -> {
                    if (failure instanceof CancellationException) action.accept(first, second);
                });
    }

    private static <T, U> void redundant(
            CompletableFuture<?> caller, T first, U second, BiConsumer<T, U> action) {
        caller.whenComplete(
                (ignored, failure) -> {
                    if (caller.isCancelled()) action.accept(first, second);
                });
    }

    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 200; i++) {
            var caller = new CompletableFuture<>();
            var first = new CompletableFuture<>();
            var second = new CompletableFuture<>();
            reference(caller, first, second, CANCEL);
            redundant(caller, first, second, CANCEL);
            ZLinkCompletionBridge.forwardCancellation(caller, first, second);
            caller.complete(null);
        }
        long[] required = captures(0);
        long[] positive = captures(1);
        long[] actual = captures(2);
        System.out.printf(
                "requests=%d requiredObjects=%d requiredBytes=%d positiveObjects=%d"
                        + " positiveBytes=%d actualObjects=%d actualBytes=%d%n",
                REQUESTS, required[0], required[1], positive[0], positive[1], actual[0], actual[1]);
        assertEquals(REQUESTS, required[0]);
        assertEquals(REQUESTS, positive[0]);
        assertEquals(REQUESTS, actual[0]);
        assertTrue(positive[1] > required[1], "the extra captured caller must be detected");
        assertEquals(required[1], actual[1], "only forwarding targets and action need capture");
    }

    private static long[] captures(int variant) throws Exception {
        var file = Files.createTempFile("zlink-cancellation-capture-", ".jfr");
        try {
            try (var recording = new Recording()) {
                recording.enable("jdk.ObjectAllocationOutsideTLAB").withStackTrace();
                recording.enable("jdk.DataLoss");
                recording.start();
                for (int i = 0; i < REQUESTS; i++) {
                    var caller = new CompletableFuture<>();
                    var first = new CompletableFuture<>();
                    var second = new CompletableFuture<>();
                    if (variant == 0) reference(caller, first, second, CANCEL);
                    else if (variant == 1) redundant(caller, first, second, CANCEL);
                    else ZLinkCompletionBridge.forwardCancellation(caller, first, second);
                    caller.complete(null);
                }
                recording.stop();
                recording.dump(file);
            }
            long objects = 0, bytes = 0;
            try (var events = new RecordingFile(file)) {
                while (events.hasMoreEvents()) {
                    var event = events.readEvent();
                    assertNotEquals("jdk.DataLoss", event.getEventType().getName());
                    if (!event.getEventType().getName().equals("jdk.ObjectAllocationOutsideTLAB")
                            || !event.getClass("objectClass").getName().contains("$$Lambda"))
                        continue;
                    if (event.getStackTrace() != null
                            && event.getStackTrace().getFrames().stream()
                                    .anyMatch(
                                            frame ->
                                                    frame.getMethod().getName().equals("reference")
                                                            || frame.getMethod()
                                                                    .getName()
                                                                    .equals("redundant")
                                                            || (frame.getMethod()
                                                                            .getType()
                                                                            .getName()
                                                                            .equals(
                                                                                    ZLinkCompletionBridge
                                                                                            .class
                                                                                            .getName())
                                                                    && frame.getMethod()
                                                                            .getName()
                                                                            .equals(
                                                                                    "forwardCancellation")))) {
                        objects++;
                        bytes += event.getLong("allocationSize");
                    }
                }
            }
            return new long[] {objects, bytes};
        } finally {
            Files.deleteIfExists(file);
        }
    }
}
