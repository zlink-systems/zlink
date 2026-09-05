/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.*;
import systems.zlink.contracts.eventing.*;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.*;
import systems.zlink.contracts.sockets.*;

class RouterReceiveStorageContractTest {
    @Test
    void blockingAndNoWaitPreserveMultipartRepliesAndStorageOnNoData() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             DealerSocket dealer = context.createDealerSocket();
             RouterSocket router = context.createRouterSocket();
             Poller input = Zlink.createPoller();
             Poller completions = Zlink.createPoller();
             Received received = new Received()) {
            router.options().recvTimeout(Duration.ZERO);
            String endpoint = TestSupport.inprocEndpoint("router-receive-storage");
            router.bind(endpoint);
            dealer.connect(endpoint);
            input.add(router, 1, PollEventFlags.POLLIN);
            completions.add(dealer, 2, PollEventFlags.POLLCOMPLETION);
            PollEvents events = new PollEvents(1);
            for (RecvFlags flags : new RecvFlags[] {RecvFlags.NONE, RecvFlags.DONT_WAIT}) {
                for (int count : new int[] {1, 2, 3, 9}) {
                    RequestSubmitOperation request = dealer.request()
                        .message(Message.from("part-0"))
                        .timeout(Duration.ofSeconds(2));
                    for (int i = 1; i < count; i++)
                        request.message(Message.from("part-" + i));
                    var future = request.submit().toCompletableFuture();
                    assertEquals(1, input.wait(events, Duration.ofSeconds(2)));
                    assertTrue(router.recv(received, flags));
                    assertEquals(count, received.parts().size());
                    assertTrue(received.getRoutingId().isPresent());
                    assertTrue(received.replyToken().isPresent());
                    Message first = received.firstPart();
                    if (flags == RecvFlags.NONE) {
                        ZlinkRecvException empty = assertThrows(ZlinkRecvException.class,
                            () -> router.recv(received, flags));
                        assertEquals(RecvResult.NO_DATA, empty.getResult());
                    } else {
                        assertFalse(router.recv(received, flags));
                    }
                    assertSame(first, received.firstPart());
                    assertEquals("part-0", first.toUtf8String());
                    assertEquals(count, received.parts().size());
                    ReplySubmitOperation reply = received.reply().message(first);
                    for (int i = 1; i < count; i++)
                        reply.message(received.parts().get(i));
                    reply.submit();
                    received.close();
                    while (!future.isDone())
                        completions.wait(events, Duration.ofSeconds(2));
                    List<Message> returned = future.join();
                    try {
                        assertEquals(count, returned.size());
                        for (int i = 0; i < count; i++)
                            assertEquals("part-" + i, returned.get(i).toUtf8String());
                    } finally {
                        Message.closeAll(returned);
                    }
                }
            }
        }
    }
}
