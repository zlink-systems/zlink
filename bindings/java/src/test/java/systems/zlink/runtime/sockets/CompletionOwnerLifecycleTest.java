/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;

class CompletionOwnerLifecycleTest {
    @Test
    void ownerlessAsyncRequestFailsFastWithoutSubmission() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket();
             Received received = new Received()) {
            String endpoint = TestSupport.inprocEndpoint("ownerless-request");
            router.bind(endpoint);
            dealer.connect(endpoint);
            try (Message ready = Message.from("ready")) {
                dealer.send().message(ready).submit_sync();
            }
            assertTrue(router.recv(received, RecvFlags.NONE));
            received.close();

            try (Message request = Message.from("not-submitted")) {
                ZlinkSubmitException failure = assertThrows(
                    ZlinkSubmitException.class, () -> dealer.request()
                        .message(request).timeout(Duration.ofSeconds(2))
                        .submit());
                assertEquals(SubmitResult.INVALID_STATE, failure.getResult());
                assertEquals("not-submitted", request.toUtf8String());
            }
            assertFalse(router.recv(received, RecvFlags.DONT_WAIT));
        }
    }

    @Test
    void publicPollerReleaseRemovesCompletionOwner() throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket();
             Poller poller = Zlink.createPoller();
             Received received = new Received()) {
            String endpoint = TestSupport.inprocEndpoint("public-handover");
            router.bind(endpoint);
            dealer.connect(endpoint);
            poller.add(dealer, 1, PollEventFlags.POLLCOMPLETION);

            var first = dealer.request().message(Message.from("first"))
                .timeout(Duration.ofSeconds(2)).submit().reply()
                .toCompletableFuture();
            assertTrue(router.recv(received, RecvFlags.NONE));
            received.reply().message(Message.from("one")).submit();
            received.close();
            assertEquals(1, poller.wait(new PollEvents(1),
                Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
            closeReply(first.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));

            assertTrue(poller.remove(dealer));
            try (Message request = Message.from("ownerless")) {
                ZlinkSubmitException failure = assertThrows(
                    ZlinkSubmitException.class, () -> dealer.request()
                        .message(request).timeout(Duration.ofSeconds(2))
                        .submit());
                assertEquals(SubmitResult.INVALID_STATE, failure.getResult());
            }

            poller.add(dealer, 2, PollEventFlags.POLLCOMPLETION);
            var second = dealer.request().message(Message.from("second"))
                .timeout(Duration.ofSeconds(2)).submit().reply()
                .toCompletableFuture();
            assertTrue(router.recv(received, RecvFlags.NONE));
            received.reply().message(Message.from("two")).submit();
            received.close();
            assertEquals(1, poller.wait(new PollEvents(1),
                Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS)));
            closeReply(second.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));
        }
    }

    @Test
    void ownerlessAsyncSendFailsFastWhenBackpressured() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             DealerSocket sender = context.createDealerSocket();
             DealerSocket receiver = context.createDealerSocket()) {
            context.options().autoHwmEnabled(false);
            sender.options().sendHwm(512L);
            receiver.options().recvHwm(512L);
            String endpoint = TestSupport.inprocEndpoint("ownerless-send");
            receiver.bind(endpoint);
            sender.connect(endpoint);

            for (int index = 0; index < 1_000; index++) {
                try (Message message = new Message(256)) {
                    try {
                        sender.send().message(message).submit();
                    } catch (ZlinkSubmitException failure) {
                        assertEquals(SubmitResult.INVALID_STATE,
                            failure.getResult());
                        assertEquals(256, message.size());
                        return;
                    }
                }
            }
            throw new AssertionError("send did not reach ownerless backpressure");
        }
    }

    private static void closeReply(List<Message> reply) {
        Message.closeAll(reply);
    }
}
