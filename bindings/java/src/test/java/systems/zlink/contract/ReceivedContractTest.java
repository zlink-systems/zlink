package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;
import java.nio.charset.StandardCharsets;
import java.util.List;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class ReceivedContractTest {
    @Test
    public void recvReturnsAggregateWithRoutingIdAndMultipartView() {
        TestSupport.assumeNative();

        RoutingId dealerRid = RoutingId.from("dealer-a".getBytes(StandardCharsets.UTF_8));
        try (Context ctx = Zlink.createContext();
             RouterSocket router = ctx.createRouterSocket();
             DealerSocket dealer = ctx.createDealerSocket()) {
            dealer.setRoutingId(dealerRid);
            String endpoint = TestSupport.inprocEndpoint("received-contract");
            router.bind(endpoint);
            dealer.connect(endpoint);

            dealer.send()
                .message(Message.from("part-1"))
                .message(Message.from("part-2"))
                .submit()
                .toCompletableFuture().join();

            try (systems.zlink.contracts.messaging.Received inbound = new systems.zlink.contracts.messaging.Received()) {


                router.recv(inbound, systems.zlink.contracts.sockets.RecvFlags.NONE);
                assertTrue(inbound.getRoutingId().isPresent());
                assertArrayEquals(dealerRid.toBytes(),
                    inbound.getRoutingId().orElseThrow().toBytes());
                assertEquals(2, inbound.parts().size());
                assertFalse(inbound.isSinglePart());
                assertThrows(UnsupportedOperationException.class,
                    () -> inbound.parts().add(Message.from("x")));
                assertArrayEquals("part-1".getBytes(StandardCharsets.UTF_8),
                    inbound.firstPart().toByteArray());
                assertTrue(inbound.requestSeq().isEmpty());
                ZlinkSubmitException ex = assertThrows(ZlinkSubmitException.class,
                    () -> inbound.reply()
                        .message(Message.from("ack"))
                        .submit());
                assertEquals(SubmitResult.INVALID_STATE, ex.getResult());
            }
        }
    }
}
