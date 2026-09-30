/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.ZlinkTimer;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;

public class PollerRemovalCompletionContractTest {
    @Test
    public void removingEarlierTimerKeepsLaterCompletionOwner() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket();
             RouterSocket router = context.createRouterSocket();
             ZlinkTimer timer = Zlink.createTimer();
             Poller poller = Zlink.createPoller();
             Received received = new Received()) {
            String endpoint = TestSupport.inprocEndpoint("poller-remove-before-completion");
            router.bind(endpoint);
            dealer.connect(endpoint);
            poller.add(timer, 1);
            poller.add(dealer, 2, PollEventFlags.POLLCOMPLETION);
            assertTrue(poller.remove(timer));

            var reply = dealer.request().message(Message.from("request"))
                .timeout(Duration.ofSeconds(2)).submit().reply().toCompletableFuture();
            assertTrue(router.recv(received, RecvFlags.NONE));
            received.reply().message(received.firstPart()).submit();
            received.close();

            PollEvents events = new PollEvents(1);
            assertEquals(1, poller.wait(events, Duration.ofSeconds(2)));
            assertEquals(2, events.slot(0));
            assertTrue(reply.isDone());
            List<Message> parts = reply.join();
            try {
                assertEquals("request", parts.get(0).toUtf8String());
            } finally {
                Message.closeAll(parts);
            }
        }
    }
}
