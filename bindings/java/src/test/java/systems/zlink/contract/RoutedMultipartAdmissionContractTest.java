/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;

class RoutedMultipartAdmissionContractTest {
    @Test
    void multipartRequestUsesOneExactNonblockingRecordAttempt()
        throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket();
             ExecutorService server = Executors.newSingleThreadExecutor()) {
            String endpoint = TestSupport.inprocEndpoint(
                "routed-multipart-admission");
            dealer.setRoutingId(RoutingId.from("multipart-client"));
            router.bind(endpoint);
            dealer.connect(endpoint);

            var serverResult = server.submit(() -> {
                try (Received request = new Received()) {
                    router.recv(request, RecvFlags.NONE);
                    // Core 0.13.0 currently has a multipart async ROUTER
                    // abort/DEALER generic-target defect. Keep this async
                    // contract probe to one part until that Core fix lands.
                    assertEquals(List.of("first"), request.parts()
                        .stream().map(Message::toUtf8String).toList());
                    request.reply()
                        .message(Message.from("reply-first"))
                        .submit();
                }
            });

            List<Message> reply = dealer.request()
                .message(Message.from("first"))
                .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                .submit()
                .toCompletableFuture()
                .get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            try {
                assertEquals(List.of("reply-first"), reply
                    .stream().map(Message::toUtf8String).toList());
            } finally {
                Message.closeAll(reply);
            }
            serverResult.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS);
        }
    }
}
