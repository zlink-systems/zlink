/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;

class RequestTerminalContractTest {
    @Test
    void synchronousReturnTerminalReturnsOwnedReply() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             DealerSocket client = context.createDealerSocket();
             ExecutorService worker = Executors.newSingleThreadExecutor()) {
            connect(server, client, "request-sync-return");
            worker.submit(() -> replyOnce(server, "pong"));

            List<Message> reply = client.request().message(Message.from("ping"))
                .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                .submit_sync(SendFlags.NONE);
            try {
                assertEquals("pong", reply.getFirst().toUtf8String());
            } finally {
                Message.closeAll(reply);
            }
        }
    }

    @Test
    void callbackTerminalReturnsAfterAdmissionAndDeliversReply()
        throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             DealerSocket client = context.createDealerSocket();
             ExecutorService worker = Executors.newSingleThreadExecutor()) {
            connect(server, client, "request-sync-callback");
            worker.submit(() -> replyOnce(server, "pong"));
            CountDownLatch completed = new CountDownLatch(1);
            AtomicReference<RequestResult> result = new AtomicReference<>();
            AtomicReference<List<Message>> reply = new AtomicReference<>();

            client.request().message(Message.from("ping"))
                .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                .submit_sync(SendFlags.NONE, (value, parts) -> {
                    result.set(value);
                    reply.set(parts);
                    completed.countDown();
                });

            org.junit.jupiter.api.Assertions.assertTrue(completed.await(
                TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
            assertEquals(RequestResult.OK, result.get());
            try {
                assertEquals("pong", reply.get().getFirst().toUtf8String());
            } finally {
                Message.closeAll(reply.get());
            }
        }
    }

    @Test
    void callbackDontWaitReportsAdmissionBackpressureSynchronously() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             DealerSocket client = context.createDealerSocket()) {
            client.options().sendHwm(1L);
            server.options().recvHwm(1L);
            connect(server, client, "request-sync-backpressure");

            ZlinkSubmitException failure = null;
            for (int attempt = 0; attempt < 10_000 && failure == null;
                 attempt++) {
                try {
                    client.request().message(Message.from("fill-" + attempt))
                        .timeout(Duration.ofSeconds(30))
                        .submit_sync(SendFlags.DONT_WAIT,
                            (result, parts) -> Message.closeAll(parts));
                } catch (ZlinkSubmitException error) {
                    failure = error;
                }
            }
            assertNotNull(failure, "DONT_WAIT must stop at the full HWM");
            assertEquals(SubmitResult.BACKPRESSURED, failure.getResult());
        }
    }

    private static void connect(RouterSocket server, DealerSocket client,
                                String name) {
        String endpoint = TestSupport.inprocEndpoint(name);
        client.setRoutingId(RoutingId.from(name));
        server.bind(endpoint);
        client.connect(endpoint);
    }

    private static void replyOnce(RouterSocket server, String value) {
        try (Received received = new Received()) {
            server.recv(received, RecvFlags.NONE);
            received.reply().message(Message.from(value)).submit();
        }
    }
}
