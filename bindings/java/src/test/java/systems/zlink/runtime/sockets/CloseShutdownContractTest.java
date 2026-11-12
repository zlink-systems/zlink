/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.runtime.nativeapi.Native;

class CloseShutdownContractTest {
    // Core returns SHUTDOWN when a close of the same handle was already
    // accepted (core socket spec close errors). The socket is then finished.
    @Test
    void shutdownCloseSettlesPendingRequestBeforeThrowing() throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket();
             Poller poller = Zlink.createPoller();
             Received received = new Received()) {
            String endpoint = TestSupport.inprocEndpoint("shutdown-close");
            router.bind(endpoint);
            dealer.connect(endpoint);
            poller.add(dealer, 1, PollEventFlags.POLLCOMPLETION);
            CompletableFuture<?> pending = dealer.request()
                .message(Message.from("pending"))
                .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                .submit().reply().toCompletableFuture();
            assertTrue(router.recv(received, RecvFlags.NONE));
            received.close();
            assertTrue(poller.remove(dealer));

            NativeSocketRuntime runtime = ((NativeSocketBase) dealer).runtime();
            assertEquals(CloseResult.OK.value(), Native.close(runtime.handle()));

            ZlinkCloseException close = assertThrows(ZlinkCloseException.class,
                dealer::close);
            assertEquals(CloseResult.SHUTDOWN, close.getResult());
            assertTrue(pending.isDone());
            ExecutionException failure = assertThrows(ExecutionException.class,
                pending::get);
            ZlinkRequestException request = assertInstanceOf(
                ZlinkRequestException.class, failure.getCause());
            assertEquals(RequestResult.TERMINATED, request.getResult());
            assertDoesNotThrow(dealer::close);
        }
    }
}
