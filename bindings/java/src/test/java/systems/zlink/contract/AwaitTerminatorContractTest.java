/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;

/** Covers {@link systems.zlink.contracts.messaging.TimeoutSubmitOperation#await()}. */
class AwaitTerminatorContractTest {
    @Test
    void awaitBlocksUntilTheReplyArrives() throws Exception {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             RouterSocket server = ctx.createRouterSocket();
             DealerSocket client = ctx.createDealerSocket();
             ExecutorService serverExecutor = Executors.newSingleThreadExecutor()) {
            String endpoint = TestSupport.inprocEndpoint("await-terminator-reply");
            client.setRoutingId(RoutingId.from("await-client"));
            server.bind(endpoint);
            client.connect(endpoint);

            serverExecutor.submit(() -> {
                try (Received received = new Received()) {
                    server.recv(received, RecvFlags.NONE);
                    received.reply().message(Message.from("pong")).submit();
                }
            });

            List<Message> reply;
            try (Message request = Message.from("ping")) {
                reply = client.request()
                    .message(request)
                    .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                    .await();
            }
            try {
                assertEquals(1, reply.size());
                assertEquals("pong", reply.getFirst().toUtf8String());
            } finally {
                Message.closeAll(reply);
            }
        }
    }

    @Test
    void awaitRethrowsTheUnwrappedFailureInsteadOfCompletionException() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             RouterSocket server = ctx.createRouterSocket();
             DealerSocket client = ctx.createDealerSocket()) {
            // The peer is connected, so submit() succeeds synchronously and
            // returns a CompletionStage; nobody ever replies, so the
            // request completes exceptionally later, on the CompletionStage
            // (join() wraps that in a CompletionException that await() must
            // unwrap).
            String endpoint = TestSupport.inprocEndpoint("await-terminator-timeout");
            client.setRoutingId(RoutingId.from("await-timeout-client"));
            server.bind(endpoint);
            client.connect(endpoint);

            ZlinkRequestException failure;
            try (Message request = Message.from("ping")) {
                failure = assertThrows(ZlinkRequestException.class, () ->
                    client.request()
                        .message(request)
                        .timeout(Duration.ofMillis(200))
                        .await());
            }
            assertEquals(RequestResult.TIMED_OUT, failure.getResult());
            // The runtime settles pending request bookkeeping shortly after
            // the timeout fires; closing immediately can race that cleanup.
            TestSupport.allowTcpRequestReplyCallbackHandshakeToSettle();
        }
    }
}
