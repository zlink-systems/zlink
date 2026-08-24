package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.XSubSocket;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.SubmitResult;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class ReceivedContractTest {
    @Test
    public void receiveSurfaceHasNoLeaseEraEntryPoints() {
        for (Class<?> socketType : List.of(PairSocket.class,
            DealerSocket.class, RouterSocket.class, StreamSocket.class,
            SubSocket.class, XSubSocket.class)) {
            assertFalse(Arrays.stream(socketType.getMethods())
                .anyMatch(method -> method.getName().equals("recvRetained")
                    || method.getName().equals("subscribeRetained")));
        }
    }

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

    @Test
    public void dealerOrdinaryRecvPreservesTypedRequestSequence()
        throws Exception {
        TestSupport.assumeNative();

        RoutingId dealerRid = RoutingId.from("dealer-ordinary-recv");
        try (Context ctx = Zlink.createContext();
             RouterSocket router = ctx.createRouterSocket();
             DealerSocket dealer = ctx.createDealerSocket()) {
            dealer.setRoutingId(dealerRid);
            String endpoint = TestSupport.inprocEndpoint(
                "dealer-ordinary-recv");
            router.bind(endpoint);
            dealer.connect(endpoint);

            try (Message ready = Message.from("ready")) {
                dealer.send().message(ready).submit()
                    .toCompletableFuture().join();
            }
            try (Received ready = new Received()) {
                assertTrue(router.recv(ready, RecvFlags.NONE));
                assertArrayEquals(dealerRid.toBytes(), ready.getRoutingId()
                    .orElseThrow().toBytes());
            }

            try (Message ordinary = Message.from("ordinary")) {
                router.send(dealerRid).message(ordinary).submit()
                    .toCompletableFuture().join();
            }

            try (Received request = new Received()) {
                assertTrue(dealer.recv(request, RecvFlags.NONE));
                assertEquals("ordinary",
                    request.singlePartOrThrow().toUtf8String());
                assertTrue(request.requestSeq().isEmpty());

                var completion = router.request(dealerRid)
                    .message(Message.from("typed-request"))
                    .timeout(Duration.ofMillis(200))
                    .submit();
                assertTrue(dealer.recv(request, RecvFlags.NONE));
                assertEquals("typed-request",
                    request.singlePartOrThrow().toUtf8String());
                assertTrue(request.requestSeq().isPresent());

                ExecutionException timedOut = assertThrows(
                    ExecutionException.class, () -> completion
                        .toCompletableFuture()
                        .get(TestSupport.DEFAULT_TIMEOUT_MS,
                            TimeUnit.MILLISECONDS));
                ZlinkRequestException failure =
                    (ZlinkRequestException) timedOut.getCause();
                assertEquals(RequestResult.TIMED_OUT, failure.getResult());
            }
        }
    }
}
