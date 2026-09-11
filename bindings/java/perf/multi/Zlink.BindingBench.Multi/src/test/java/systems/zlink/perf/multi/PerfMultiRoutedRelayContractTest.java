/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

class PerfMultiRoutedRelayContractTest {
    @Test
    void receivedPartsAreConsumedByDirectRelayAndEnvelopeCloseIsRepeatSafe()
        throws Exception {
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket();
             RouterSocket router = context.createRouterSocket();
             Received routed = new Received();
             Received echoed = new Received()) {
            String endpoint = "inproc://perf-java-routed-relay-"
                + System.nanoTime();
            dealer.options().recvTimeout(Duration.ofSeconds(2));
            router.options().recvTimeout(Duration.ofSeconds(2));
            router.bind(endpoint);
            dealer.connect(endpoint);

            dealer.send()
                .message(Message.from("payload"))
                .message(new Message(0))
                .submit_sync();
            assertTrue(router.recv(routed, RecvFlags.NONE));

            RoutingId source = routed.getRoutingId().orElseThrow();
            List<Message> sourceParts = routed.parts();
            var admission = PerfMultiRoutedRelay.submitReply(
                router, source, sourceParts);

            assertEquals(0, sourceParts.get(0).size());
            assertEquals(0, sourceParts.get(1).size());
            routed.close();
            routed.close();
            if (admission.result() == SubmitResult.BACKPRESSURED) {
                admission.admitted().toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            } else {
                assertEquals(SubmitResult.OK, admission.result());
            }

            assertTrue(dealer.recv(echoed, RecvFlags.NONE));
            assertEquals(2, echoed.parts().size());
            assertEquals("payload", echoed.parts().get(0).toUtf8String());
            assertEquals(0, echoed.parts().get(1).size());
        }
    }
}
