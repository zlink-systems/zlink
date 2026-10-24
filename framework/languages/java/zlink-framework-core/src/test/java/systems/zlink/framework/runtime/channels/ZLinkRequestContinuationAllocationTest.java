package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.service.ZLinkServiceSendHeaderAllocationTest;

import java.util.List;
import java.util.concurrent.CompletableFuture;

/** Distinguishes executable completion nodes from terminal wrappers and stage views. */
public final class ZLinkRequestContinuationAllocationTest {
    private static final int REQUESTS = 1000;
    private static volatile Object observed;

    @Test
    void countsCompletionNodesWithPositiveAndNegativeControls() throws Exception {
        ZLinkServiceSendHeaderAllocationTest.runInAllocationJvm(getClass());
    }

    private static void negative() {
        observed =
                CompletableFuture.failedFuture(new IllegalStateException())
                        .minimalCompletionStage();
    }

    private static final class OwnedFuture extends CompletableFuture<Object> {}

    private static void positive() {
        var future = new OwnedFuture();
        observed = future.whenComplete((value, failure) -> {});
        future.complete(null);
    }

    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 200; i++) {
            negative();
            positive();
        }
        var negative =
                ZLinkRequestCallerAllocationTest.allocations(
                        ZLinkRequestContinuationAllocationTest.class,
                        List.of("negative"),
                        () -> {
                            for (int i = 0; i < REQUESTS; i++) negative();
                        });
        var positive =
                ZLinkRequestCallerAllocationTest.allocations(
                        ZLinkRequestContinuationAllocationTest.class,
                        List.of("positive"),
                        () -> {
                            for (int i = 0; i < REQUESTS; i++) positive();
                        });
        System.out.printf(
                "requests=%d nonContinuationControl=%s completionControl=%s%n",
                REQUESTS, negative, positive);
        assertEquals(
                0,
                negative.continuations(),
                "failed result and minimal stage are not executable continuations");
        assertEquals(
                2L * REQUESTS, negative.futures(), "include concrete and minimal stage futures");
        assertEquals(
                REQUESTS,
                positive.continuations(),
                "one actual whenComplete node per pending future");
        assertEquals(2L * REQUESTS, positive.futures());
    }
}
