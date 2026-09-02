/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.Arrays;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SocketType;

final class CompletionOwnershipContractTest {
    @Test
    void provisionalValidationPreservesEveryCallerMessage() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             NativeSocketRuntime dealer = new NativeSocketRuntime(context,
                 SocketType.DEALER);
             Message first = Message.from("first")) {
            NullPointerException failure = assertThrows(
                NullPointerException.class, () -> dealer.submitSend(null,
                    Arrays.asList(first, null)));
            assertEquals("parts[1]", failure.getMessage());
            assertEquals("first", first.toUtf8String());
            assertFalse(first.more());
        }
    }

    @Test
    void failedStagingLeavesPrefixReusable() throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             NativeSocketRuntime sender = new NativeSocketRuntime(context,
                 SocketType.PAIR);
             NativeSocketRuntime receiver = new NativeSocketRuntime(context,
                 SocketType.PAIR);
             Message first = Message.from("first");
             Message closed = Message.from("closed")) {
            String endpoint = TestSupport.inprocEndpoint("completion-staging");
            receiver.bind(endpoint);
            sender.connect(endpoint);
            closed.close();
            assertThrows(IllegalStateException.class, () ->
                sender.submitSend(null, List.of(first, closed)));
            assertEquals("first", first.toUtf8String());
            sender.submitSend(null, List.of(first)).toCompletableFuture().get(
                TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        }
    }

    @Test
    void cancelledWaitStillClosesOneLateNativeCompletion() throws Exception {
        TestSupport.assumeNative();
        long before = CompletionOwner.closedCompletionCount();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket();
             RouterSocket router = context.createRouterSocket();
             Received request = new Received()) {
            String endpoint = TestSupport.inprocEndpoint(
                "late-completion-cleanup");
            router.bind(endpoint);
            dealer.connect(endpoint);
            var stage = dealer.request().message(Message.from("timeout"))
                .timeout(Duration.ofMillis(20)).submit().toCompletableFuture();
            router.recv(request, RecvFlags.NONE);
            assertFalse(stage.isDone());
            stage.cancel(false);

            long deadline = System.nanoTime()
                + TimeUnit.SECONDS.toNanos(2);
            while (CompletionOwner.closedCompletionCount() == before
                    && System.nanoTime() < deadline) {
                Thread.sleep(1L);
            }
            assertEquals(before + 1,
                CompletionOwner.closedCompletionCount());
            Thread.sleep(25L);
            assertEquals(before + 1,
                CompletionOwner.closedCompletionCount());
        }
    }
}
