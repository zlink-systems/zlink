/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import java.time.Duration;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.function.Executable;
import systems.zlink.CompletionPollerDriver;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.SubmitResult;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

final class ConsumedMessageSubmitContractTest {
    @Test
    void rejectsConsumedMessageAcrossSubmitOperations() throws Exception {
        TestSupport.assumeNative();
        try (var context = Zlink.createContext();
             var dealer = context.createDealerSocket();
             var router = context.createRouterSocket();
             var pub = context.createPubSocket();
             var completions = new CompletionPollerDriver(dealer);
             var received = new Received();
             var consumed = Message.from("probe");
             var first = Message.from("preserved")) {
            var endpoint = TestSupport.inprocEndpoint("consumed-message");
            router.bind(endpoint);
            dealer.connect(endpoint);
            dealer.send().message(consumed).submit_sync();
            assertTrue(router.recv(received, RecvFlags.NONE));
            assertEquals("probe", received.firstPart().toUtf8String());

            var pending = dealer.request().message(Message.from("ping"))
                .timeout(Duration.ofSeconds(5)).submit();
            assertTrue(router.recv(received, RecvFlags.NONE));
            assertAll(
                () -> assertInvalidHandle(() ->
                    dealer.send().message(consumed).submit_sync()),
                () -> assertInvalidHandle(() ->
                    dealer.send().message(first).message(consumed).submit()),
                () -> assertInvalidHandle(() ->
                    dealer.request().message(consumed).submit_sync()),
                () -> assertInvalidHandle(() ->
                    dealer.request().message(consumed).submit()),
                () -> assertInvalidHandle(() ->
                    received.reply().message(consumed).submit()),
                () -> assertInvalidHandle(() ->
                    pub.publish("topic").message(consumed).submit()),
                () -> assertInvalidHandle(() ->
                    pub.publish("topic").message(first).message(consumed).submit()),
                () -> assertEquals(0, consumed.size()),
                () -> assertEquals(0, consumed.data().length),
                () -> assertEquals("preserved", first.toUtf8String()));
            received.reply().message(Message.from("pong")).submit();
            var reply = pending.reply().toCompletableFuture().get(
                TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            try {
                assertEquals("pong", reply.getFirst().toUtf8String());
            } finally {
                Message.closeAll(reply);
            }
            assertFalse(router.recv(received, RecvFlags.DONT_WAIT));
        }
    }

    private static void assertInvalidHandle(Executable submit) {
        assertEquals(SubmitResult.INVALID_HANDLE,
            assertThrows(ZlinkSubmitException.class, submit).getResult());
    }
}
